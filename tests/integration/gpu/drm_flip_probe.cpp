// SPDX-License-Identifier: GPL-2.0-or-later
// Page-flip pacing of the Virtio-GPU display.
//
// Virtio-GPU has no vertical blank of its own, so the driver paces each CRTC
// with a DRM vblank timer at the refresh rate of the mode. The probe takes the
// first connected output of the virtio_gpu card, sets its preferred mode, and
//   - waits for 30 vblanks with DRM_IOCTL_WAIT_VBLANK, and
//   - flips two dumb buffers for two seconds, one flip in flight at a time,
//     asking for a completion event each time, and times the events.
// A paced display completes flips at the refresh rate of the mode; a CRTC with
// no vblank completes each flip as it is queued. Both rates must be within 10%
// of the refresh rate.
//
// The timer runs only while something holds a vblank reference and stops at
// the first tick without one. When a reference is taken again it must keep
// the grid it ran on: the first tick is a whole number of periods after the
// last one, not a period after the time the reference was taken. The core
// counts the vblanks of the gap from the timestamps, so the count and the
// timestamp must stay a pair on that grid. After the rate checks the probe
// lets the display go idle and takes a reference again, which restarts the
// timer, with the gap and the way to ask varied:
//   - a short gap (1.5 to 3 periods) and a long one (1 s and 3 s);
//   - 100 gaps of different lengths in a row;
//   - a page flip after a gap, whose completion event is the tick;
//   - a mode change, after which the timer may start a new grid, and the way
//     back to the preferred mode.
// Every restart asks twice: for the current vblank (a relative request for
// sequence 0), which returns the vblank the core counted when it enabled the
// timer, and for the next one. Both timestamps must be grid points of the grid
// the probe measured before the gap, and the sequence numbers must advance by
// one per period of that grid (the sequence number of a vblank is the
// reference one plus the periods since the reference). Timestamps are exact to
// the nanosecond in the kernel and reach user space in microseconds,
// truncated; the difference of two truncations is under 1 us, so a stamp is on
// the grid when it is within 2 us of a grid point.
// Last, the vblank events the kernel traced (tracefs, drm_vblank_event) show
// that the timer was off while the display was idle: between the end of an
// activity and the next, in a gap longer than three periods, the kernel
// reports at most one tick, the one at which the timer stops, however late the
// host runs it, and that every tick of a grid is exactly on it.
//
// The probe prints one PASS or FAIL line per check and ends with
// UML_DRM_VBLANK_PROBE: PASS or FAIL, exiting 0 only after PASS.
//
// Build for the guest architecture with c++ -std=c++20 -O2 -Wall -Wextra
// -Werror -static.
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

// The connector status of a connected output, which libdrm names and the kernel
// headers do not.
constexpr uint32_t connector_connected = 1;

constexpr int64_t second_ns = 1000000000;
// A timestamp is on a grid when it is within this of a grid point.
constexpr int64_t stamp_tolerance_ns = 2000;

void report(const char *name, bool ok, const std::string &detail)
{
	std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
	if (!ok)
		++failures;
}

std::string errno_text(int e)
{
	return std::to_string(e) + " " + std::strerror(e);
}

std::string format(const char *text, ...) __attribute__((format(printf, 1, 2)));

std::string format(const char *text, ...)
{
	char buffer[512];
	va_list arguments;
	va_start(arguments, text);
	std::vsnprintf(buffer, sizeof buffer, text, arguments);
	va_end(arguments);
	return buffer;
}

int64_t abs64(int64_t value)
{
	return value < 0 ? -value : value;
}

int64_t mono_ns()
{
	timespec t{};
	clock_gettime(CLOCK_MONOTONIC, &t);
	return static_cast<int64_t>(t.tv_sec) * second_ns + t.tv_nsec;
}

double now_ms()
{
	return static_cast<double>(mono_ns()) / 1e6;
}

void sleep_ns(int64_t ns)
{
	if (ns <= 0)
		return;
	int64_t deadline = mono_ns() + ns;
	timespec t{};
	t.tv_sec = deadline / second_ns;
	t.tv_nsec = deadline % second_ns;
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr) == EINTR) {
	}
}

int ioctl_retry(int fd, unsigned long request, void *argument)
{
	int result;
	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && (errno == EINTR || errno == EAGAIN));
	return result;
}

// The first card whose driver is virtio_gpu.
int open_virtio_gpu()
{
	for (int i = 0; i < 8; ++i) {
		std::string path = "/dev/dri/card" + std::to_string(i);
		int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		char name[64] = {};
		drm_version version{};
		version.name = name;
		version.name_len = sizeof name - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 &&
		    std::strcmp(name, "virtio_gpu") == 0) {
			std::printf("INFO card %s\n", path.c_str());
			return fd;
		}
		close(fd);
	}
	return -1;
}

struct Output {
	uint32_t connector = 0;
	uint32_t crtc = 0;
	std::vector<drm_mode_modeinfo> modes;
};

bool find_output(int fd, Output &out)
{
	drm_mode_card_res resources{};
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0)
		return false;
	std::vector<uint32_t> crtcs(resources.count_crtcs), connectors(resources.count_connectors),
		encoders(resources.count_encoders), fbs(resources.count_fbs);
	resources.crtc_id_ptr = reinterpret_cast<uintptr_t>(crtcs.data());
	resources.connector_id_ptr = reinterpret_cast<uintptr_t>(connectors.data());
	resources.encoder_id_ptr = reinterpret_cast<uintptr_t>(encoders.data());
	resources.fb_id_ptr = reinterpret_cast<uintptr_t>(fbs.data());
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0)
		return false;
	for (uint32_t connector_id : connectors) {
		drm_mode_get_connector connector{};
		connector.connector_id = connector_id;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0)
			continue;
		std::vector<drm_mode_modeinfo> modes(connector.count_modes);
		std::vector<uint32_t> connector_encoders(connector.count_encoders);
		connector.modes_ptr = reinterpret_cast<uintptr_t>(modes.data());
		connector.encoders_ptr = reinterpret_cast<uintptr_t>(connector_encoders.data());
		connector.count_props = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0 ||
		    connector.connection != connector_connected || modes.empty())
			continue;
		drm_mode_get_encoder encoder{};
		encoder.encoder_id = connector.encoder_id ? connector.encoder_id
							  : (connector_encoders.empty() ? 0 : connector_encoders[0]);
		if (encoder.encoder_id == 0 || ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0)
			continue;
		uint32_t crtc = encoder.crtc_id;
		for (size_t i = 0; crtc == 0 && i < crtcs.size(); ++i)
			if (encoder.possible_crtcs & (1u << i))
				crtc = crtcs[i];
		if (crtc == 0)
			continue;
		out.connector = connector_id;
		out.crtc = crtc;
		out.modes = modes;
		return true;
	}
	return false;
}

struct Buffer {
	uint32_t handle = 0;
	uint32_t fb = 0;
};

bool make_buffer(int fd, const drm_mode_modeinfo &mode, uint8_t fill, Buffer &buffer)
{
	drm_mode_create_dumb create{};
	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
		return false;
	buffer.handle = create.handle;
	drm_mode_fb_cmd2 fb{};
	fb.width = mode.hdisplay;
	fb.height = mode.vdisplay;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = create.handle;
	fb.pitches[0] = create.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0)
		return false;
	buffer.fb = fb.fb_id;
	drm_mode_map_dumb map{};
	map.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
		return true; // the pixels are not what is measured
	void *pixels = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			    static_cast<off_t>(map.offset));
	if (pixels != MAP_FAILED) {
		std::memset(pixels, fill, create.size);
		munmap(pixels, create.size);
	}
	return true;
}

bool set_mode(int fd, const Output &output, const drm_mode_modeinfo &mode, uint32_t fb)
{
	uint32_t connector = output.connector;
	drm_mode_crtc set{};
	set.crtc_id = output.crtc;
	set.fb_id = fb;
	set.set_connectors_ptr = reinterpret_cast<uintptr_t>(&connector);
	set.count_connectors = 1;
	set.mode_valid = 1;
	set.mode = mode;
	return ioctl_retry(fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
}

double median(std::vector<double> values)
{
	if (values.empty())
		return 0;
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

// The duration of a frame, as the DRM core computes it from the mode: the
// frame size in pixels over the pixel clock in kHz, in whole nanoseconds.
int64_t frame_ns(const drm_mode_modeinfo &mode)
{
	return static_cast<int64_t>(static_cast<uint64_t>(mode.htotal) * mode.vtotal * 1000000u /
				    mode.clock);
}

// A vblank as user space reads it: a sequence number and the time of that
// vblank on CLOCK_MONOTONIC, truncated to microseconds.
struct Stamp {
	int64_t seq = 0;
	int64_t ns = 0;
};

bool vblank_request(int fd, uint32_t crtc_index, uint32_t type, uint32_t sequence, Stamp &out,
		    int &error)
{
	drm_wait_vblank vblank{};
	vblank.request.type = static_cast<drm_vblank_seq_type>(
		type | ((crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK));
	vblank.request.sequence = sequence;
	if (ioctl_retry(fd, DRM_IOCTL_WAIT_VBLANK, &vblank) != 0) {
		error = errno;
		return false;
	}
	out.seq = vblank.reply.sequence;
	out.ns = static_cast<int64_t>(vblank.reply.tval_sec) * second_ns +
		 static_cast<int64_t>(vblank.reply.tval_usec) * 1000;
	return true;
}

// A grid of vblanks: one reference vblank and the period; the vblank n periods
// after it has the sequence number of the reference plus n.
struct Grid {
	int64_t period = 0;
	Stamp ref;
};

struct StampCheck {
	int64_t residual = 0;  // distance from the nearest grid point, ns
	int64_t seq_error = 0; // sequence number minus the number the time implies
	bool ok = false;
};

int64_t nearest_periods(int64_t delta, int64_t period)
{
	return (delta >= 0 ? delta + period / 2 : delta - period / 2) / period;
}

StampCheck check_stamp(const Grid &grid, const Stamp &stamp)
{
	StampCheck check;
	int64_t delta = stamp.ns - grid.ref.ns;
	int64_t periods = nearest_periods(delta, grid.period);
	check.residual = delta - periods * grid.period;
	check.seq_error = (stamp.seq - grid.ref.seq) - periods;
	check.ok = abs64(check.residual) <= stamp_tolerance_ns && check.seq_error == 0;
	return check;
}

// One restart of the vblank timer: let the display idle for `gap_ns`, then
// take a reference by asking for the current vblank and for the next one.
struct Restart {
	int64_t gap_ns = 0;
	int64_t t0 = 0, t1 = 0, t2 = 0; // before the query, after it, after the wait
	Stamp query, next;
	int error = 0;
};

// A stretch in which the display was idle, for the trace check: the time of
// the last activity and the time the next one began.
struct IdleWindow {
	int64_t from = 0;
	int64_t to = 0;
	const char *label = "";
};

struct Probe {
	int fd = -1;
	uint32_t crtc_index = 0;
	int64_t last_activity = 0;
	const char *label = "";
	std::vector<IdleWindow> idle;
	// Where the grid changes by design (a mode change): the times, taken while
	// the timer is stopped.
	std::vector<int64_t> grid_changes;
};

bool run_restart(Probe &probe, int64_t gap_ns, Restart &r)
{
	r.gap_ns = gap_ns;
	sleep_ns(gap_ns);
	r.t0 = mono_ns();
	probe.idle.push_back({probe.last_activity, r.t0, probe.label});
	if (!vblank_request(probe.fd, probe.crtc_index, _DRM_VBLANK_RELATIVE, 0, r.query, r.error))
		return false;
	r.t1 = mono_ns();
	if (!vblank_request(probe.fd, probe.crtc_index, _DRM_VBLANK_RELATIVE, 1, r.next, r.error))
		return false;
	r.t2 = mono_ns();
	probe.last_activity = r.t2;
	return true;
}

// What a restart must show beyond the grid: the query returns the latest
// vblank, which is not after the call and less than a period before it, and
// the next vblank is after the call began.
std::string range_error(const Grid &grid, const Restart &r)
{
	if (r.query.ns > r.t1)
		return format("the queried vblank (%lld) is after the call returned (%lld)",
			      static_cast<long long>(r.query.ns), static_cast<long long>(r.t1));
	if (r.query.ns + 2000 < r.t0 - grid.period)
		return format("the queried vblank is %lld ns before the call, over a period",
			      static_cast<long long>(r.t0 - r.query.ns));
	if (r.next.seq <= r.query.seq)
		return format("the next vblank has sequence %lld after %lld",
			      static_cast<long long>(r.next.seq), static_cast<long long>(r.query.seq));
	if (r.next.ns + 2000 < r.t0)
		return "the next vblank is before the call";
	if (r.next.ns > r.t2)
		return "the next vblank is after the call returned";
	return std::string();
}

struct Tally {
	int restarts = 0;
	int bad = 0;
	int64_t worst_residual = 0;
	int64_t worst_seq_error = 0;
	std::string first_bad;
};

void tally_check(Tally &tally, const StampCheck &check)
{
	tally.worst_residual = std::max(tally.worst_residual, abs64(check.residual));
	tally.worst_seq_error = std::max(tally.worst_seq_error, abs64(check.seq_error));
}

void tally_restart(Tally &tally, const Grid &grid, const Restart &r)
{
	++tally.restarts;
	std::string problem;
	StampCheck q = check_stamp(grid, r.query);
	StampCheck n = check_stamp(grid, r.next);
	tally_check(tally, q);
	tally_check(tally, n);
	if (!q.ok)
		problem = format("query: %.3f ms off the grid, sequence off by %lld",
				 static_cast<double>(q.residual) / 1e6, static_cast<long long>(q.seq_error));
	else if (!n.ok)
		problem = format("next: %.3f ms off the grid, sequence off by %lld",
				 static_cast<double>(n.residual) / 1e6, static_cast<long long>(n.seq_error));
	else
		problem = range_error(grid, r);
	if (!problem.empty()) {
		++tally.bad;
		if (tally.first_bad.empty())
			tally.first_bad = format("restart %d after a gap of %.2f ms: %s", tally.restarts,
						 static_cast<double>(r.gap_ns) / 1e6, problem.c_str());
	}
}

std::string tally_text(const Tally &tally)
{
	std::string text =
		format("%d restarts, worst distance from the grid %.3f us, worst sequence error %lld",
		       tally.restarts, static_cast<double>(tally.worst_residual) / 1e3,
		       static_cast<long long>(tally.worst_seq_error));
	if (!tally.first_bad.empty())
		text += format("; %d bad, the first: %s", tally.bad, tally.first_bad.c_str());
	return text;
}

// Run the gaps in a row against one grid.
Tally run_series(Probe &probe, const Grid &grid, const std::vector<int64_t> &gaps, int &failed_ioctl)
{
	Tally tally;
	failed_ioctl = 0;
	for (int64_t gap : gaps) {
		Restart r;
		if (!run_restart(probe, gap, r)) {
			failed_ioctl = r.error;
			break;
		}
		tally_restart(tally, grid, r);
	}
	return tally;
}

// A page flip with a completion event; returns the event's stamp.
bool flip_once(int fd, const Output &output, const Buffer &buffer, uint64_t user_data, Stamp &event,
	       int64_t &issued, int64_t &received, int &error)
{
	drm_mode_crtc_page_flip flip{};
	flip.crtc_id = output.crtc;
	flip.fb_id = buffer.fb;
	flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
	flip.user_data = user_data;
	issued = mono_ns();
	if (ioctl_retry(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) {
		error = errno;
		return false;
	}
	for (;;) {
		pollfd waiter{fd, POLLIN, 0};
		if (poll(&waiter, 1, 1000) != 1) {
			error = ETIMEDOUT;
			return false;
		}
		char data[256];
		ssize_t got = read(fd, data, sizeof data);
		if (got < static_cast<ssize_t>(sizeof(drm_event))) {
			error = EIO;
			return false;
		}
		for (ssize_t at = 0; at + static_cast<ssize_t>(sizeof(drm_event)) <= got;) {
			auto *base = reinterpret_cast<drm_event *>(data + at);
			if (base->type == DRM_EVENT_FLIP_COMPLETE &&
			    base->length >= sizeof(drm_event_vblank)) {
				auto *vblank = reinterpret_cast<drm_event_vblank *>(base);
				if (vblank->user_data == user_data) {
					event.seq = vblank->sequence;
					event.ns = static_cast<int64_t>(vblank->tv_sec) * second_ns +
						   static_cast<int64_t>(vblank->tv_usec) * 1000;
					received = mono_ns();
					return true;
				}
			}
			if (base->length == 0)
				break;
			at += base->length;
		}
	}
}

// tracefs: the vblank events the kernel traced.
struct TraceEvent {
	int64_t seq = 0;
	int64_t ns = 0;
};

std::string read_file(const std::string &path)
{
	std::string text;
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return text;
	char data[65536];
	for (;;) {
		ssize_t got = read(fd, data, sizeof data);
		if (got <= 0)
			break;
		text.append(data, static_cast<size_t>(got));
	}
	close(fd);
	return text;
}

bool write_file(const std::string &path, const char *text, int flags = O_WRONLY)
{
	int fd = open(path.c_str(), flags | O_CLOEXEC);
	if (fd < 0)
		return false;
	size_t length = std::strlen(text);
	bool ok = length == 0 || write(fd, text, length) == static_cast<ssize_t>(length);
	close(fd);
	return ok;
}

// Start tracing drm_vblank_event; returns the tracefs directory, or empty.
std::string start_trace()
{
	const char *candidates[] = {"/sys/kernel/tracing", "/sys/kernel/debug/tracing"};
	for (int attempt = 0; attempt < 2; ++attempt) {
		for (const char *dir : candidates) {
			std::string base = dir;
			std::string enable = base + "/events/drm/drm_vblank_event/enable";
			if (access(enable.c_str(), W_OK) != 0)
				continue;
			write_file(base + "/tracing_on", "0");
			write_file(base + "/trace", "", O_WRONLY | O_TRUNC);
			if (!write_file(enable, "1"))
				continue;
			write_file(base + "/tracing_on", "1");
			return base;
		}
		if (attempt == 0) {
			// tracefs may not be mounted yet.
			int ignored = std::system("mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null");
			(void)ignored;
		}
	}
	return std::string();
}

std::vector<TraceEvent> read_trace(const std::string &dir)
{
	write_file(dir + "/tracing_on", "0");
	std::string text = read_file(dir + "/trace");
	std::vector<TraceEvent> events;
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		at = end + 1;
		if (line.find("drm_vblank_event:") == std::string::npos)
			continue;
		size_t seq = line.find("seq=");
		size_t time = line.find("time=");
		if (seq == std::string::npos || time == std::string::npos)
			continue;
		TraceEvent event;
		event.seq = std::strtoll(line.c_str() + seq + 4, nullptr, 10);
		event.ns = std::strtoll(line.c_str() + time + 5, nullptr, 10);
		events.push_back(event);
	}
	std::sort(events.begin(), events.end(),
		  [](const TraceEvent &a, const TraceEvent &b) { return a.ns < b.ns; });
	return events;
}

// A mode change in the middle of the run: the display idles first so that
// the timer is stopped when the time of the change is taken, then the mode is
// set, then the first vblank of the new grid becomes its reference, and
// restarts are checked against it. The count must not run away.
struct ModeChange {
	bool ok = false;
	std::string detail;
};

ModeChange change_mode(Probe &probe, const Output &output, const drm_mode_modeinfo &from_mode,
		       const drm_mode_modeinfo &to_mode, uint32_t fb, bool record_series)
{
	ModeChange result;
	int64_t from_period = frame_ns(from_mode);
	int error = 0;
	// A reference, then idle until the timer has stopped.
	Stamp before;
	int64_t began = mono_ns();
	if (!vblank_request(probe.fd, probe.crtc_index, _DRM_VBLANK_RELATIVE, 0, before, error)) {
		result.detail = "query before the change: " + errno_text(error);
		return result;
	}
	probe.idle.push_back({probe.last_activity, began, probe.label});
	probe.last_activity = mono_ns();
	sleep_ns(from_period * 4);
	probe.idle.push_back({probe.last_activity, mono_ns(), probe.label});
	probe.grid_changes.push_back(mono_ns());
	if (!set_mode(probe.fd, output, to_mode, fb)) {
		result.detail = "modeset: " + errno_text(errno);
		return result;
	}
	Grid grid;
	grid.period = frame_ns(to_mode);
	if (!vblank_request(probe.fd, probe.crtc_index, _DRM_VBLANK_RELATIVE, 1, grid.ref, error)) {
		result.detail = "first vblank: " + errno_text(error);
		return result;
	}
	probe.last_activity = mono_ns();
	int64_t elapsed = probe.last_activity - began;
	int64_t bound = elapsed / std::min(from_period, grid.period) + 4;
	if (grid.ref.seq - before.seq < 0 || grid.ref.seq - before.seq > bound) {
		result.detail = format("the sequence went from %lld to %lld in %.1f ms",
				       static_cast<long long>(before.seq), static_cast<long long>(grid.ref.seq),
				       static_cast<double>(elapsed) / 1e6);
		return result;
	}
	if (record_series) {
		int series_error = 0;
		Tally tally = run_series(probe, grid,
					 {grid.period * 5 / 2, grid.period * 3 / 10, 3 * grid.period,
					  300 * 1000 * 1000},
					 series_error);
		result.ok = series_error == 0 && tally.bad == 0;
		result.detail = series_error ? errno_text(series_error) : tally_text(tally);
	} else {
		result.ok = true;
		result.detail = "mode set";
	}
	return result;
}

} // namespace

int main(int argc, char **)
{
	if (argc != 1) {
		std::fprintf(stderr, "usage: drm_flip_probe\n");
		return 125;
	}
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	int fd = open_virtio_gpu();
	if (fd < 0) {
		report("virtio-gpu", false, "no virtio_gpu card in /dev/dri");
		return failures;
	}
	ioctl(fd, DRM_IOCTL_SET_MASTER, nullptr);
	Output output;
	if (!find_output(fd, output)) {
		report("output", false, "no connected output");
		return failures;
	}
	const drm_mode_modeinfo mode = output.modes[0];
	double refresh_hz = static_cast<double>(mode.clock) * 1000.0 /
			    (static_cast<double>(mode.htotal) * static_cast<double>(mode.vtotal));
	int64_t period = frame_ns(mode);
	std::printf("INFO mode %ux%u refresh %.2f Hz, period %lld ns\n", mode.hdisplay, mode.vdisplay,
		    refresh_hz, static_cast<long long>(period));
	for (size_t i = 0; i < output.modes.size(); ++i)
		std::printf("INFO mode %zu: %ux%u period %lld ns\n", i, output.modes[i].hdisplay,
			    output.modes[i].vdisplay, static_cast<long long>(frame_ns(output.modes[i])));
	Buffer buffers[2];
	if (!make_buffer(fd, mode, 0x40, buffers[0]) || !make_buffer(fd, mode, 0xc0, buffers[1])) {
		report("buffers", false, errno_text(errno));
		return failures;
	}
	if (!set_mode(fd, output, mode, buffers[0].fb)) {
		report("modeset", false, errno_text(errno));
		return failures;
	}
	report("modeset", true, "preferred mode on the first output");

	// A bare CRTC index for the vblank requests: the position of the CRTC in
	// the resource list.
	uint32_t crtc_index = 0;
	{
		drm_mode_card_res resources{};
		ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
		std::vector<uint32_t> crtcs(resources.count_crtcs);
		resources.crtc_id_ptr = reinterpret_cast<uintptr_t>(crtcs.data());
		resources.count_connectors = resources.count_encoders = resources.count_fbs = 0;
		ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
		for (size_t i = 0; i < crtcs.size(); ++i)
			if (crtcs[i] == output.crtc)
				crtc_index = static_cast<uint32_t>(i);
	}

	drm_get_cap cap{};
	cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
	if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0 || cap.value != 1)
		report("clock", false, "vblank timestamps are not on CLOCK_MONOTONIC");

	Probe probe;
	probe.fd = fd;
	probe.crtc_index = crtc_index;
	int64_t trace_begin = mono_ns();
	std::string trace_dir = start_trace();
	if (trace_dir.empty())
		report("trace", false, "drm_vblank_event cannot be traced (no tracefs)");
	else
		std::printf("INFO tracing in %s\n", trace_dir.c_str());

	// Thirty vblanks, one at a time.
	bool wait_ok = true;
	int wait_errno = 0;
	std::vector<Stamp> waited;
	double wait_begin = now_ms();
	for (int i = 0; i < 30 && wait_ok; ++i) {
		Stamp stamp;
		if (vblank_request(fd, crtc_index, _DRM_VBLANK_RELATIVE, 1, stamp, wait_errno))
			waited.push_back(stamp);
		else
			wait_ok = false;
	}
	double wait_hz = 30.0 * 1000.0 / (now_ms() - wait_begin);
	report("wait-vblank", wait_ok && wait_hz > refresh_hz * 0.9 && wait_hz < refresh_hz * 1.1,
	       wait_ok ? "30 vblanks at " + std::to_string(wait_hz) + " Hz" : errno_text(wait_errno));
	probe.last_activity = mono_ns();

	// The grid the timer runs on while it runs: every vblank of the waits is a
	// grid point, a period apart.
	Grid grid;
	grid.period = period;
	bool continuous = waited.size() == 30;
	std::string continuous_detail = "30 vblanks";
	if (continuous) {
		grid.ref = waited.back();
		int64_t worst = 0;
		for (const Stamp &stamp : waited) {
			StampCheck check = check_stamp(grid, stamp);
			worst = std::max(worst, abs64(check.residual));
			if (!check.ok)
				continuous = false;
		}
		continuous_detail =
			format("30 vblanks, worst distance from the grid %.3f us, period %lld ns",
			       static_cast<double>(worst) / 1e3, static_cast<long long>(period));
	}
	report("grid-continuous", continuous, continuous_detail);

	// Two seconds of flips, one in flight at a time.
	std::vector<double> arrivals;
	std::vector<double> stamps;
	std::vector<double> issues;
	std::vector<int64_t> stamp_ns;
	int flipped = 0;
	bool flip_ok = true;
	int flip_errno = 0;
	double begin = now_ms();
	while (now_ms() - begin < 2000.0 && flip_ok) {
		Buffer &next = buffers[(flipped + 1) % 2];
		Stamp event;
		int64_t issued = 0, received = 0;
		if (!flip_once(fd, output, next, static_cast<uint64_t>(flipped), event, issued, received,
			       flip_errno)) {
			flip_ok = false;
			break;
		}
		stamps.push_back(static_cast<double>(event.ns) / 1e6);
		stamp_ns.push_back(event.ns);
		issues.push_back(static_cast<double>(issued) / 1e6);
		arrivals.push_back(static_cast<double>(received) / 1e6);
		++flipped;
	}
	probe.last_activity = mono_ns();
	double flip_hz = flipped > 1 ? (flipped - 1) * 1000.0 / (arrivals.back() - arrivals.front()) : 0;
	std::vector<double> gaps;
	for (size_t i = 1; i < stamps.size(); ++i)
		gaps.push_back(stamps[i] - stamps[i - 1]);
	char summary[160];
	std::snprintf(summary, sizeof summary,
		      "%d flips at %.1f Hz, event gap median %.2f ms, min %.2f ms, max %.2f ms",
		      flipped, flip_hz, median(gaps),
		      gaps.empty() ? 0.0 : *std::min_element(gaps.begin(), gaps.end()),
		      gaps.empty() ? 0.0 : *std::max_element(gaps.begin(), gaps.end()));
	report("flip-paced", flip_ok && flip_hz > refresh_hz * 0.9 && flip_hz < refresh_hz * 1.1,
	       flip_ok ? summary : errno_text(flip_errno));
	// Which side was late in the largest gap: the probe issuing the flip, the
	// commit and the tick completing it, or the event being read. The ticks the
	// kernel traced inside the gap are printed with the trace, below.
	int64_t flip_gap_from = 0, flip_gap_to = 0;
	if (flip_ok && !gaps.empty()) {
		size_t i = static_cast<size_t>(std::max_element(gaps.begin(), gaps.end()) - gaps.begin()) + 1;
		std::printf("INFO largest flip gap %.2f ms at flip %zu: issued %.2f ms after the previous event was read, stamped %.2f ms after it was issued, read %.2f ms after its stamp\n",
			    gaps[i - 1], i, issues[i] - arrivals[i - 1], stamps[i] - issues[i],
			    arrivals[i] - stamps[i]);
		flip_gap_from = stamp_ns[i - 1];
		flip_gap_to = stamp_ns[i];
	}
	int shown = flipped % 2; // the buffer the display shows now is buffers[shown]

	// Restarts after a gap. A gap of 1.5 periods ends between the tick at
	// which the timer stopped and the one at which it fires for the last time;
	// the others end after it stopped for good.
	int failed_ioctl = 0;
	probe.label = "1.5-periods";
	Tally tally = run_series(probe, grid, {period * 3 / 2}, failed_ioctl);
	report("restart-after-1.5-periods", failed_ioctl == 0 && tally.bad == 0,
	       failed_ioctl ? errno_text(failed_ioctl) : tally_text(tally));
	probe.label = "short-gap";
	tally = run_series(probe, grid, {period * 22 / 10, period * 5 / 2, period * 28 / 10, period * 3},
			   failed_ioctl);
	report("restart-short-gap", failed_ioctl == 0 && tally.bad == 0,
	       failed_ioctl ? errno_text(failed_ioctl) : tally_text(tally));
	probe.label = "long-gap";
	tally = run_series(probe, grid, {3 * second_ns, second_ns}, failed_ioctl);
	report("restart-long-gap", failed_ioctl == 0 && tally.bad == 0,
	       failed_ioctl ? errno_text(failed_ioctl) : tally_text(tally));
	probe.label = "repeated";
	{
		std::vector<int64_t> varied;
		for (int i = 0; i < 100; ++i) {
			double fraction = std::fmod(i * 0.6180339887498949, 1.0);
			varied.push_back(static_cast<int64_t>(static_cast<double>(period) *
							      (1.25 + 3.5 * fraction)));
		}
		tally = run_series(probe, grid, varied, failed_ioctl);
		report("restart-repeated", failed_ioctl == 0 && tally.restarts == 100 && tally.bad == 0,
		       failed_ioctl ? errno_text(failed_ioctl) : tally_text(tally));
	}

	// A flip after a gap completes on a grid point after the commit.
	probe.label = "flip";
	{
		Tally flips;
		int flip_failed = 0;
		for (int64_t gap : {period * 5 / 2, 3 * second_ns, period * 3 / 2, period * 27 / 10}) {
			sleep_ns(gap);
			Stamp event;
			int64_t issued = 0, received = 0;
			probe.idle.push_back({probe.last_activity, mono_ns(), probe.label});
			shown = (shown + 1) % 2;
			if (!flip_once(fd, output, buffers[shown], 1000, event, issued, received,
				       flip_failed))
				break;
			probe.last_activity = received;
			++flips.restarts;
			StampCheck check = check_stamp(grid, event);
			tally_check(flips, check);
			std::string problem;
			if (!check.ok)
				problem = format("%.3f ms off the grid, sequence off by %lld",
						 static_cast<double>(check.residual) / 1e6,
						 static_cast<long long>(check.seq_error));
			else if (event.ns + 2000 < issued)
				problem = "the event is before the flip was queued";
			else if (event.ns > received)
				problem = "the event is after it was received";
			if (!problem.empty()) {
				++flips.bad;
				if (flips.first_bad.empty())
					flips.first_bad = format("flip %d after a gap of %.2f ms: %s", flips.restarts,
								 static_cast<double>(gap) / 1e6, problem.c_str());
			}
		}
		report("flip-event-grid", flip_failed == 0 && flips.restarts == 4 && flips.bad == 0,
		       flip_failed ? errno_text(flip_failed) : tally_text(flips));
	}

	// A mode change may start a new grid.
	{
		// The smallest mode of the same rate (within 2%) and of another
		// rate: a small frame buffer keeps the change cheap.
		const drm_mode_modeinfo *other_size = nullptr;
		const drm_mode_modeinfo *other_rate = nullptr;
		auto area = [](const drm_mode_modeinfo &m) {
			return static_cast<uint64_t>(m.hdisplay) * m.vdisplay;
		};
		for (size_t i = 1; i < output.modes.size(); ++i) {
			const drm_mode_modeinfo &candidate = output.modes[i];
			const drm_mode_modeinfo *&slot =
				abs64(frame_ns(candidate) - period) * 100 > period * 2 ? other_rate : other_size;
			if (candidate.hdisplay != mode.hdisplay && (!slot || area(candidate) < area(*slot)))
				slot = &candidate;
		}
		std::vector<std::pair<const char *, const drm_mode_modeinfo *>> changes = {
			{"mode-change-size", other_size}, {"mode-change-rate", other_rate}};
		bool any = false;
		for (auto &[name, target] : changes) {
			if (!target)
				continue;
			any = true;
			Buffer extra;
			probe.label = name;
			if (!make_buffer(fd, *target, 0x80, extra)) {
				report(name, false, "no buffer for the mode: " + errno_text(errno));
				continue;
			}
			ModeChange there = change_mode(probe, output, mode, *target, extra.fb, true);
			// The way back to the preferred mode starts a grid of its own.
			ModeChange back = change_mode(probe, output, *target, mode, buffers[shown].fb, true);
			report(name, there.ok && back.ok,
			       "to " + std::to_string(target->hdisplay) + "x" +
				       std::to_string(target->vdisplay) + ": " + there.detail +
				       "; back: " + back.detail);
		}
		if (!any)
			std::printf("INFO no second mode to change to\n");
	}

	// The kernel's own record: no tick while the display was idle, and every
	// tick of a grid exactly on it.
	if (!trace_dir.empty()) {
		std::vector<TraceEvent> events = read_trace(trace_dir);
		std::printf("INFO traced %zu vblank events\n", events.size());
		if (flip_gap_to > flip_gap_from) {
			// The flip events carry microsecond stamps, the trace nanoseconds.
			int inside = 0;
			for (const TraceEvent &event : events)
				if (event.ns >= flip_gap_from - stamp_tolerance_ns &&
				    event.ns <= flip_gap_to + stamp_tolerance_ns) {
					std::printf("INFO traced tick in the largest flip gap: sequence %lld at %+.3f ms\n",
						    static_cast<long long>(event.seq),
						    static_cast<double>(event.ns - flip_gap_from) / 1e6);
					++inside;
				}
			std::printf("INFO %d traced ticks in the largest flip gap of %.2f ms\n", inside,
				    static_cast<double>(flip_gap_to - flip_gap_from) / 1e6);
		}
		int64_t guard = 2 * 1000 * 1000;
		int64_t longest = period;
		for (const drm_mode_modeinfo &candidate : output.modes)
			longest = std::max(longest, frame_ns(candidate));
		int checked = 0, ticking = 0;
		std::string first_ticking;
		for (const IdleWindow &window : probe.idle) {
			// The activity dropped its vblank reference before it ended, so
			// the kernel reports one more tick, the one at which the timer
			// stops: a callback the host holds up reports the latest grid
			// point, once, and a tick handled earlier is stamped earlier. A
			// timer that kept running reports two or more in three periods.
			int64_t from = window.from;
			int64_t to = window.to - guard;
			if (to - from < 3 * longest)
				continue;
			++checked;
			int count = 0;
			const TraceEvent *first = nullptr;
			for (const TraceEvent &event : events)
				if (event.ns > from && event.ns < to) {
					if (!first)
						first = &event;
					++count;
				}
			if (count > 1) {
				++ticking;
				if (first_ticking.empty())
					for (const TraceEvent &event : events)
						if (event.ns > window.from - 3 * longest && event.ns < window.to + longest)
							std::printf("INFO trace around the first idle gap with a tick: sequence %lld at %+.3f ms (gap %.3f ms from the end of the activity)\n",
								    static_cast<long long>(event.seq),
								    static_cast<double>(event.ns - window.from) / 1e6,
								    static_cast<double>(window.to - window.from) / 1e6);
				if (first_ticking.empty())
					first_ticking = format(
						"%d ticks in %.2f ms of idle display (%s), the first %.2f ms after the activity ended, sequence %lld",
						count, static_cast<double>(to - from) / 1e6, window.label,
						static_cast<double>(first->ns - window.from) / 1e6,
						static_cast<long long>(first->seq));
			}
		}
		report("trace-idle", checked > 0 && ticking == 0 && !events.empty(),
		       format("%d idle gaps longer than three periods, %d with a tick beyond the stopping one%s%s",
			      checked, ticking, first_ticking.empty() ? "" : ": ", first_ticking.c_str()));

		// Segments between mode changes: within one, every tick is a whole
		// number of periods from the first, and its sequence number moved by
		// as many.
		std::vector<int64_t> bounds = {trace_begin};
		for (int64_t change : probe.grid_changes)
			bounds.push_back(change);
		bounds.push_back(INT64_MAX);
		int segments = 0, bad_events = 0, counted = 0;
		std::string first_bad;
		for (size_t s = 0; s + 1 < bounds.size(); ++s) {
			std::vector<TraceEvent> segment;
			for (const TraceEvent &event : events)
				if (event.ns >= bounds[s] && event.ns < bounds[s + 1])
					segment.push_back(event);
			if (segment.size() < 3)
				continue;
			// The kernel's period is the distance of the first two ticks of
			// the segment that follow each other.
			int64_t segment_period = 0;
			for (size_t i = 1; i < segment.size(); ++i)
				if (segment[i].seq == segment[i - 1].seq + 1) {
					segment_period = segment[i].ns - segment[i - 1].ns;
					break;
				}
			if (segment_period <= 0)
				continue;
			++segments;
			for (const TraceEvent &event : segment) {
				++counted;
				int64_t delta = event.ns - segment[0].ns;
				int64_t periods = nearest_periods(delta, segment_period);
				int64_t residual = delta - periods * segment_period;
				int64_t seq_error = (event.seq - segment[0].seq) - periods;
				if (residual != 0 || seq_error != 0) {
					++bad_events;
					if (first_bad.empty())
						first_bad = format("tick %lld at %lld is %lld ns off the grid, sequence off by %lld",
								   static_cast<long long>(event.seq),
								   static_cast<long long>(event.ns),
								   static_cast<long long>(residual),
								   static_cast<long long>(seq_error));
				}
			}
			std::printf("INFO grid %zu: %zu ticks, period %lld ns\n", s, segment.size(),
				    static_cast<long long>(segment_period));
		}
		report("trace-grid", segments > 0 && bad_events == 0,
		       format("%d ticks in %d grids, %d off the grid%s%s", counted, segments, bad_events,
			      first_bad.empty() ? "" : ": ", first_bad.c_str()));
	}

	std::printf("UML_DRM_VBLANK_PROBE: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
