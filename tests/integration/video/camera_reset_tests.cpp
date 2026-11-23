// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Virtio camera driver against a camera device that is reset during a
 * suspend, and against one that is late to stop a stream.
 *
 * The production emulator cannot be made to reset a device, and no switch in a
 * production target may, so this serves the guest's camera with the production
 * device and transport behind the delayed-answer shim of tests/support/vhost_user.
 * The camera is the synthetic one (tablet profile). A guest streams the first
 * camera's video and metadata with the zoom written, suspends itself with an
 * RTC alarm, and checks what V4L2 shows it (camera_reset_init.cpp). The case
 * names what the host does meanwhile:
 *
 *   reset       The first GET_PARAMS after a stream has been cleared (the
 *               driver's question on resume) makes the shim reset the wrapped
 *               device first, as the transport does to a device it resets
 *               during a suspend: every stream, control value and capture is
 *               gone. The driver must say so once, create its streams again,
 *               write the zoom again, and keep the sequences of the frames and
 *               the metadata rising.
 *
 *   slow-clear  The device hears of a stream's QUEUE_CLEAR seven seconds late,
 *               after the driver's five-second wait, as it would behind slow
 *               work. The driver must destroy the stream instead of waiting
 *               for it, so that the buffers it gives back to the client (at the
 *               end of the run, the client fills them with a pattern) are never
 *               written again. The late clears fall due while the guest is
 *               suspended, with its rings disabled, and the device must not be
 *               reset by that: every answer is still delivered afterwards.
 *
 *   slow-stop   The device hears of a stream's QUEUE_CLEAR twelve seconds late
 *               and of the STREAM_DESTROY the driver sends after its wait just
 *               as late, with the buffers still at the device (a metadata
 *               stream with no exposure to fill them). Both of the driver's
 *               waits run out, and the buffers must still not go back to the
 *               client until the device has done its work: the stop returns
 *               only once the clear has reached the device.
 *
 * A process boots one guest, so the run is a child process of this one, with
 * its console in a log the parent then checks.
 *
 *   uml_camera_reset_tests KERNEL INIT SCRATCH reset|slow-clear|slow-stop
 */
#include "uml/emulator.h"

#include "boot_initramfs.hpp"
#include "camera_device.hpp"
#include "delayed_answer_device.hpp"
#include "in_process_vhost_user_service.hpp"
#include "synthetic_camera/synthetic_camera.hpp"
#include "virtio_video.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <pthread.h>
#include <regex>
#include <spawn.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

using namespace std::chrono_literals;
using uml::testing::DelayedAnswerDevice;

/* The driver waits five seconds for a stream's clear. */
constexpr auto slow_clear = 7000ms;

/* Past the clear's wait, and past the destroy's: the stop is still waiting for
 * the device when the clear reaches it. */
constexpr auto slow_stop = 12000ms;

/* The synthetic camera stamps every frame this long before it delivers it, so
 * a frame's age is known: the init checks that the buffers' timestamps are at
 * least this far behind the time they are dequeued, which a driver that did
 * not subtract the age from its clock would not give. */
constexpr std::uint32_t capture_latency_ms = 300;

/* A cold boot, the suspend (a minute at most in the slow case), a final stop
 * that waits twice and the power-off. */
constexpr auto child_deadline = 300s;

constexpr std::string_view failures[]{
	"WARNING:", "BUG ", "Oops", "Call Trace:", "Kernel panic",
	"Darwin UML panic", "Kernel mode fault", "Poison overwritten",
	"Redzone", "Object already free", "Objects remaining",
	"the vhost-user backend is gone", "slave reports error",
	"vhost-user connection failed"};

constexpr char reset_line[] = "the device was reset: restoring the streams";
constexpr char destroyed_line[] =
	"the device did not stop a stream in time: destroying it";
constexpr char waiting_line[] =
	"still waiting for the device to give back a stream's buffers";

/* The guest process, which a timed-out CTest's interrupt must not orphan. */
volatile sig_atomic_t guest_process = 0;

extern "C" void stop_guest(int signal)
{
	if (guest_process > 0)
		::kill(static_cast<pid_t>(guest_process), SIGKILL);
	::_exit(128 + signal);
}

std::vector<char> read_file(const char *path)
{
	std::ifstream file{path, std::ios::binary};
	if (!file)
		throw std::runtime_error{std::string{"cannot read "} + path};
	return {std::istreambuf_iterator<char>{file}, {}};
}

/* One whole line per write, so the transport thread's lines do not split
 * the kernel's. */
void log_line(const std::string &line)
{
	const std::string text = "camera-reset: " + line + "\n";
	(void)!::write(STDOUT_FILENO, text.data(), text.size());
}

const char *command_name(std::uint32_t type)
{
	switch (type) {
	case VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE:
		return "STREAM_CREATE";
	case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
		return "STREAM_DESTROY";
	case VIRTIO_VIDEO_CMD_GET_PARAMS:
		return "GET_PARAMS";
	case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
		return "QUEUE_CLEAR";
	default:
		return nullptr;
	}
}

bool is_reset_case(std::string_view name) { return name == "reset"; }

bool is_stop_case(std::string_view name) { return name == "slow-stop"; }

/* What the shim does for the case, shared with its callbacks. */
struct Trigger final {
	std::atomic<DelayedAnswerDevice *> shim {};
	std::atomic_int clears {};
	std::atomic_bool fired {};
};

/* The command a request on the command queue carries, logged by name. */
std::optional<virtio_video_cmd_hdr> command(std::uint32_t queue,
	std::span<const std::uint8_t> request)
{
	if (queue != 0 || request.size() < sizeof(virtio_video_cmd_hdr))
		return std::nullopt;
	const auto header = uml::virtio::fromBytes<virtio_video_cmd_hdr>(request);
	if (const char *name = command_name(header.type))
		log_line(std::string{"CMD "} + name + " stream=" +
			 std::to_string(header.stream_id));
	return header;
}

[[noreturn]] void run_child(const char *kernel_path, const char *init_path,
	const std::string &scratch, std::string_view which)
{
	const bool reset_case = is_reset_case(which);
	const bool stop_case = is_stop_case(which);
	const auto kernel = read_file(kernel_path);
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.kernel_image = kernel.data(),
		.kernel_image_size = kernel.size(),
	};
	uml_emulator *created = nullptr;
	if (uml_emulator_create(&config, &created) != UML_EMULATOR_OK)
		std::_Exit(10);
	/* Like every embedder, keep the kernel's early SIGALRM off this
	 * process's own threads; the service thread inherits the mask. */
	sigset_t timer;
	sigemptyset(&timer);
	sigaddset(&timer, SIGALRM);
	if (pthread_sigmask(SIG_BLOCK, &timer, nullptr) != 0)
		std::_Exit(10);

	/* How long the guest's RTC alarm runs: past the late clears in the slow
	 * case, which fall due while the guest is suspended. */
	const std::string alarm = reset_case ? "+8\n" : "+25\n";
	const std::string initrd = scratch + "/initrd";
	const std::string disk = scratch + "/empty-disk.img";
	uml::boot::InitramfsArchive archive;
	archive.addHostFile("/init", init_path, 0755);
	std::vector<std::byte> alarm_bytes(alarm.size());
	std::memcpy(alarm_bytes.data(), alarm.data(), alarm.size());
	/* The stop case does not suspend: its init finds /stop instead. */
	if (stop_case)
		archive.addFile("/stop", std::vector<std::byte>{}, 0644, 0);
	else
		archive.addFile("/alarm", alarm_bytes, 0644, 0);
	(void)::unlink(initrd.c_str());
	uml::boot::writeNewFile(initrd, archive.serialize());
	const int descriptor = ::open(disk.c_str(),
		O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (descriptor < 0 || ::ftruncate(descriptor, 16 << 20) != 0)
		std::_Exit(10);
	::close(descriptor);

	auto trigger = std::make_shared<Trigger>();
	uml::gpu::InProcessVhostUserService service{
		"Virtio camera", "uml-camera",
		[trigger, reset_case, stop_case] {
			uml::camera::CameraDeviceOptions options;
			uml::platform::SyntheticCameraOptions camera;
			camera.profile = uml::platform::SyntheticCameraProfile::tablet;
			camera.latencyMilliseconds = capture_latency_ms;
			auto device = std::make_unique<uml::camera::VirtioCameraDevice>(
				uml::platform::makeSyntheticCamera(camera),
				std::move(options));
			/* The policy delays no answer; the case's trigger lives in
			 * the answerer, which sees every request first and answers
			 * none, and in the postponer. */
			auto shim = std::make_unique<DelayedAnswerDevice>(
				std::move(device),
				[](std::uint32_t, std::span<const std::uint8_t>)
					-> std::optional<std::chrono::milliseconds> {
					return std::nullopt;
				},
				[](std::string_view message) {
					log_line(std::string{"shim "} + std::string{message});
				},
				[trigger, reset_case](std::uint32_t queue,
					std::span<const std::uint8_t> request)
					-> std::optional<std::vector<std::uint8_t>> {
					const auto header = command(queue, request);
					if (!header)
						return std::nullopt;
					if (header->type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR)
						trigger->clears++;
					/* The driver's question on resume reaches a device
					 * that has just been reset. */
					if (reset_case &&
					    header->type == VIRTIO_VIDEO_CMD_GET_PARAMS &&
					    trigger->clears > 0 && !trigger->fired.exchange(true)) {
						log_line("forced a device reset after " +
							 std::to_string(trigger->clears.load()) +
							 " clears");
						trigger->shim.load()->prepareReset();
						trigger->shim.load()->reset();
					}
					return std::nullopt;
				});
			if (!reset_case)
				shim->setPostponer(
					[stop_case](std::uint32_t queue,
						std::span<const std::uint8_t> request)
						-> std::optional<std::chrono::milliseconds> {
						if (queue != 0 ||
						    request.size() < sizeof(virtio_video_cmd_hdr))
							return std::nullopt;
						const auto header =
							uml::virtio::fromBytes<virtio_video_cmd_hdr>(request);
						if (header.type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR)
							return stop_case ? slow_stop : slow_clear;
						/* The destroy the driver sends once its wait for
						 * the clear has run out is as late. */
						if (stop_case &&
						    header.type == VIRTIO_VIDEO_CMD_STREAM_DESTROY)
							return slow_stop;
						return std::nullopt;
					});
			trigger->shim = shim.get();
			return shim;
		}};
	const std::string device = uml::gpu::virtioDeviceArgument(
		service.takeConnectionName(), uml::camera::cameraDeviceId);
	const std::string ubd = "ubd0c=" + disk;
	const std::string initrd_argument = "initrd=" + initrd;
	const char *arguments[]{"uml-camera-reset", ubd.c_str(),
		initrd_argument.c_str(), "mem=512M", "ncpus=2",
		"slab_debug=FZPU", device.c_str()};
	const int result = uml_emulator_run(created,
		static_cast<int>(std::size(arguments)), arguments, nullptr);
	service.stop();
	log_line("run returned " + std::to_string(result));
	std::_Exit(result == 0 ? 0 : 11);
}

std::string describe(int status)
{
	if (WIFEXITED(status))
		return "exit " + std::to_string(WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		return std::string{"signal "} + strsignal(WTERMSIG(status));
	return "status " + std::to_string(status);
}

std::size_t count_of(const std::string &log, std::string_view needle)
{
	std::size_t count = 0;
	for (auto at = log.find(needle); at != std::string::npos;
	     at = log.find(needle, at + needle.size()))
		count++;
	return count;
}

/* The number after `key=` on the first line that has `prefix`; -1 if none. */
long field(const std::string &log, const std::string &prefix, const std::string &key)
{
	const std::regex pattern{prefix + "[^\n]* " + key + "=(-?[0-9]+)"};
	std::smatch match;
	if (!std::regex_search(log, match, pattern))
		return -1;
	return std::stol(match[1]);
}

int run_driver(const char *self, const char *kernel_path,
	const char *init_path, const std::string &scratch, std::string_view which)
{
	const bool reset_case = is_reset_case(which);
	const bool stop_case = is_stop_case(which);
	if (::mkdir(scratch.c_str(), 0700) != 0 && errno != EEXIST)
		throw std::runtime_error{"cannot create " + scratch};
	const std::string log_path = scratch + "/guest.log";
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
		O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
		log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
		STDERR_FILENO);
	const std::string case_name{which};
	const char *child_arguments[]{self, "--child", kernel_path, init_path,
		scratch.c_str(), case_name.c_str(), nullptr};
	pid_t child = 0;
	const int spawned = posix_spawn(&child, self, &actions, nullptr,
		const_cast<char *const *>(child_arguments), environ);
	posix_spawn_file_actions_destroy(&actions);
	if (spawned != 0)
		throw std::runtime_error{"cannot start the guest process"};
	guest_process = child;
	std::signal(SIGINT, stop_guest);
	std::signal(SIGTERM, stop_guest);

	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + child_deadline;
	bool timed_out = false;
	while (::waitpid(child, &status, WNOHANG) == 0) {
		if (std::chrono::steady_clock::now() > deadline) {
			timed_out = true;
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
			break;
		}
		std::this_thread::sleep_for(100ms);
	}

	std::ifstream file{log_path};
	std::stringstream contents;
	contents << file.rdbuf();
	const std::string log = contents.str();
	std::vector<std::string> problems;
	if (timed_out)
		problems.push_back("the guest did not stop within " +
			std::to_string(child_deadline.count()) + " s");
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		problems.push_back("the guest process ended with " + describe(status));
	for (const auto failure : failures)
		if (log.find(failure) != std::string::npos)
			problems.push_back("the guest printed \"" +
				std::string{failure} + "\"");
	if (stop_case) {
		for (const auto expected : {"CAMRESET holding", "CAMRESET streamoff ",
			     "CAMRESET PASS"})
			if (log.find(expected) == std::string::npos)
				problems.push_back(std::string{"missing \""} + expected + "\"");
	} else {
		for (const auto expected : {"CAMRESET streaming", "CAMRESET suspending", "CAMRESET ages",
			     "PM: suspend entry", "PM: suspend exit",
			     "CAMRESET resumed result=3", "CAMRESET after ",
			     "CAMRESET streamoff ", "CAMRESET PASS"})
			if (log.find(expected) == std::string::npos)
				problems.push_back(std::string{"missing \""} + expected + "\"");
	}
	if (log.find("CAMRESET FAIL") != std::string::npos)
		problems.push_back("the guest reported a failure");

	if (!stop_case) {
		/* What the client saw, whichever the case. */
		const std::string after = "CAMRESET after";
		for (const auto key : {"errors", "wrongzoom", "frameregressions",
			     "recordregressions", "recorderrors"})
			if (field(log, after, key) != 0)
				problems.push_back(std::string{"after the suspend: "} + key +
					"=" + std::to_string(field(log, after, key)));
		if (field(log, after, "matched") < 3)
			problems.push_back("after the suspend: only " +
				std::to_string(field(log, after, "matched")) +
				" frames have a metadata record of their sequence");
		if (field(log, "CAMRESET ages", "young") != 0 ||
		    field(log, "CAMRESET ages", "aged") < 20)
			problems.push_back("the frames' timestamps do not show the capture age (" +
				std::to_string(field(log, "CAMRESET ages", "young")) + " young, " +
				std::to_string(field(log, "CAMRESET ages", "aged")) + " aged)");
	}
	if (field(log, "CAMRESET streamoff", "overwritten") != 0)
		problems.push_back("a buffer the client had got back was written: " +
			std::to_string(field(log, "CAMRESET streamoff", "overwritten")) +
			" bytes");

	/* What the case did to the device. */
	const auto stopped_in = field(log, "CAMRESET streamoff", "ms");
	const auto resets = count_of(log, reset_line);
	const auto destroyed = count_of(log, destroyed_line);
	const auto forced = count_of(log, "camera-reset: forced a device reset");
	if (reset_case) {
		if (forced != 1)
			problems.push_back("the shim reset the device " +
				std::to_string(forced) + " times, not once");
		if (resets != 1)
			problems.push_back("the driver found the device reset " +
				std::to_string(resets) + " times, not once");
		if (destroyed != 0)
			problems.push_back("a stream was destroyed for want of an "
				"answer to its clear");
		if (stopped_in < 0 || stopped_in >= 3000)
			problems.push_back("STREAMOFF took " + std::to_string(stopped_in) +
				" ms with a device that answers at once");
	} else if (stop_case) {
		const auto postponed = count_of(log, "shim postponed a request for");
		const auto handed = count_of(log, "shim handed a postponed request");
		const auto waiting = count_of(log, waiting_line);
		/* The clear, then the destroy sent after the clear's wait. */
		if (postponed < 2)
			problems.push_back("only " + std::to_string(postponed) +
				" requests were postponed (a clear and a destroy)");
		if (handed < 1)
			problems.push_back("the postponed clear never reached the device");
		if (destroyed < 1)
			problems.push_back("the stream was not destroyed after its clear "
				"timed out");
		if (waiting < 1)
			problems.push_back("the stop did not say that it went on waiting");
		/* The destroy is still held when the guest powers off, and the
		 * device's reset then drops it; a drop before the run is over
		 * would lose a request the test depends on. */
		const auto dropped = log.find("shim reset dropped");
		if (dropped != std::string::npos && dropped < log.find("CAMRESET PASS"))
			problems.push_back("a reset dropped requests the device held");
		if (resets != 0 || forced != 0)
			problems.push_back("the device was reset");
		/* A stop that gave the buffers back after the clear's wait and the
		 * destroy's (ten seconds) would return before the clear reached the
		 * device. */
		if (stopped_in < 11000)
			problems.push_back("STREAMOFF took " + std::to_string(stopped_in) +
				" ms: the buffers went back before the device did its work");
		if (stopped_in > 40000)
			problems.push_back("STREAMOFF took " + std::to_string(stopped_in) +
				" ms, long after the device did its work");
	} else {
		const auto postponed = count_of(log, "shim postponed a request for");
		const auto handed = count_of(log, "shim handed a postponed request");
		if (postponed < 3)
			problems.push_back("only " + std::to_string(postponed) +
				" clears were postponed");
		/* The ones that fall due while the guest is suspended arrive after
		 * its rings are enabled again; none may be lost to a reset. */
		if (handed < 2)
			problems.push_back("only " + std::to_string(handed) +
				" postponed clears reached the device");
		if (log.find("shim reset dropped") != std::string::npos)
			problems.push_back("a reset dropped requests the device held");
		if (resets != 0)
			problems.push_back("the driver found the device reset");
		if (forced != 0)
			problems.push_back("the shim reset the device");
		if (destroyed < 2)
			problems.push_back("only " + std::to_string(destroyed) +
				" streams were destroyed after a clear timed out");
		if (stopped_in < 4000)
			problems.push_back("STREAMOFF took " + std::to_string(stopped_in) +
				" ms, not the driver's wait for a clear that does not come");
	}

	if (problems.empty()) {
		if (stop_case)
			std::printf("camera %s: the buffers stayed with the device "
				    "until it did its work\n", case_name.c_str());
		else
			std::printf("camera %s: the stream carried on across the suspend "
				    "(%s)\n", case_name.c_str(),
				    reset_case ? "the device was reset meanwhile" :
						 "the device was slow to stop a stream");
		return 0;
	}
	for (const auto &problem : problems)
		std::fprintf(stderr, "guest-video-camera-%s: %s\n",
			case_name.c_str(), problem.c_str());
	std::fprintf(stderr, "--- %s ---\n%s\n", log_path.c_str(),
		log.size() > 30000 ? log.substr(log.size() - 30000).c_str() :
		log.c_str());
	return 1;
}

} // namespace

int main(int argc, char **argv)
{
	try {
		if (argc == 6 && std::string_view{argv[1]} == "--child")
			run_child(argv[2], argv[3], argv[4], argv[5]);
		if (argc != 5 || (std::string_view{argv[4]} != "reset" &&
				  std::string_view{argv[4]} != "slow-clear" &&
				  std::string_view{argv[4]} != "slow-stop")) {
			std::fprintf(stderr,
				"usage: %s KERNEL INIT SCRATCH reset|slow-clear|slow-stop\n",
				argv[0]);
			return 2;
		}
		return run_driver(argv[0], argv[1], argv[2], argv[3], argv[4]);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "guest-video-camera-reset: %s\n", error.what());
		return 1;
	}
}
