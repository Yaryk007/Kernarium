// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux /init of guest-video-camera-reset,
// guest-video-camera-slow-clear and guest-video-camera-slow-stop.
//
// The init streams the first camera's video next to its metadata, with the
// zoom written, suspends the guest from /sys/power/state with an RTC alarm
// armed (the host's /alarm file says how long), and watches the stream carry
// on. Over V4L2 alone it checks what the client can see:
//
//   - no buffer comes back flagged as an error;
//   - the sequences of the frames, and of the metadata records, keep rising
//     across the suspend, whatever the device did meanwhile;
//   - after the suspend the metadata shows the zoom again (a run of records
//     that show the default zoom is allowed once, where the platform's new
//     session made exposures before the zoom reached it: see took_record), and
//     the records match the frames by sequence;
//   - a buffer the client has got back after STREAMOFF is never written again.
//
// What the host did to the camera device meanwhile is the test's case: it
// either made the device forget every stream during the suspend, or held the
// device's processing of a stream's clear past the driver's wait. The host
// reads the "CAMRESET" lines this prints, and the kernel's own, from the log.
//
// The slow-stop case (the host's /stop file) does not suspend. Only the
// metadata node streams, so no exposure fills its buffers and the device holds
// them until it processes the stream's clear, which the host delays past every
// wait of the driver's. The time STREAMOFF takes says whether the buffers went
// back before the device did its work, and a pattern the client writes into
// them afterwards must stay whole.

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;
using s32 = int;

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_faccessat = 48;
constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_read = 63;
constexpr long nr_write = 64;
constexpr long nr_ioctl = 29;
constexpr long nr_sync = 81;
constexpr long nr_exit_group = 94;
constexpr long nr_nanosleep = 101;
constexpr long nr_clock_gettime = 113;
constexpr long nr_reboot = 142;
constexpr long nr_mmap = 222;

constexpr long at_fdcwd = -100;
constexpr long o_rdonly = 0;
constexpr long o_wronly = 1;
constexpr long o_rdwr = 2;
constexpr long o_nonblock = 04000;
constexpr long prot_read = 1;
constexpr long prot_write = 2;
constexpr long map_shared = 1;
constexpr long eagain = 11;

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0,
		long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

template <typename T> long ptr(T *p) { return reinterpret_cast<long>(p); }

/* ---------------------------------------------------------------------- */
/* The V4L2 structures, laid out as the arm64 kernel has them              */
/* ---------------------------------------------------------------------- */

constexpr u32 buf_type_capture_mplane = 9;
constexpr u32 buf_type_meta_capture = 13;
constexpr u32 memory_mmap = 1;
constexpr u32 buf_flag_error = 0x40;
constexpr u32 pix_fmt_nv12 = 0x3231564e; /* 'N' 'V' '1' '2' */

struct Timeval {
	long sec;
	long usec;
};

struct Plane {
	u32 bytesused;
	u32 length;
	union {
		u32 mem_offset;
		u64 userptr;
		s32 fd;
	} m;
	u32 data_offset;
	u32 reserved[11];
};
static_assert(sizeof(Plane) == 64);

struct Buffer {
	u32 index;
	u32 type;
	u32 bytesused;
	u32 flags;
	u32 field;
	Timeval timestamp;
	u32 timecode[4];
	u32 sequence;
	u32 memory;
	union {
		u32 offset;
		u64 userptr;
		Plane *planes;
		s32 fd;
	} m;
	u32 length;
	u32 reserved2;
	u32 request_fd;
};
static_assert(sizeof(Buffer) == 88);

struct [[gnu::packed]] PlanePix {
	u32 sizeimage;
	u16 bytesperline;
	u16 reserved[6];
};

struct [[gnu::packed]] PixMp {
	u32 width;
	u32 height;
	u32 pixelformat;
	u32 field;
	u32 colorspace;
	PlanePix plane_fmt[8];
	u8 num_planes;
	u8 flags;
	u8 ycbcr_enc;
	u8 quantization;
	u8 xfer_func;
	u8 reserved[7];
};
static_assert(sizeof(PixMp) == 176);

struct Format {
	u32 type;
	u32 pad; /* the union that follows is 8-byte aligned */
	PixMp pix_mp;
	u8 rest[200 - sizeof(PixMp)];
};
static_assert(sizeof(Format) == 208);

struct RequestBuffers {
	u32 count;
	u32 type;
	u32 memory;
	u32 capabilities;
	u8 flags;
	u8 reserved[3];
};
static_assert(sizeof(RequestBuffers) == 20);

struct [[gnu::packed]] ExtControl {
	u32 id;
	u32 size;
	u32 reserved2;
	s32 value;
	s32 value_high;
};
static_assert(sizeof(ExtControl) == 20);

struct ExtControls {
	u32 which;
	u32 count;
	u32 error_idx;
	s32 request_fd;
	u32 reserved;
	ExtControl *controls;
};
static_assert(sizeof(ExtControls) == 32);

constexpr u32 ioc(u32 direction, u32 number, u32 size)
{
	return (direction << 30) | (size << 16) | (static_cast<u32>('V') << 8) | number;
}
constexpr u32 vidioc_s_fmt = ioc(3, 5, sizeof(Format));
constexpr u32 vidioc_reqbufs = ioc(3, 8, sizeof(RequestBuffers));
constexpr u32 vidioc_querybuf = ioc(3, 9, sizeof(Buffer));
constexpr u32 vidioc_qbuf = ioc(3, 15, sizeof(Buffer));
constexpr u32 vidioc_dqbuf = ioc(3, 17, sizeof(Buffer));
constexpr u32 vidioc_streamon = ioc(1, 18, sizeof(int));
constexpr u32 vidioc_streamoff = ioc(1, 19, sizeof(int));
constexpr u32 vidioc_s_ext_ctrls = ioc(3, 72, sizeof(ExtControls));

/* The camera's private controls and metadata record
 * (kernel/common/video/virtio_video.h). */
constexpr u32 cid_zoom_q16 = 0x00981c00 + 5;
constexpr u32 zoom_one = 0x10000;
constexpr u32 zoom_two = 0x20000;
constexpr u32 meta_size = 168;
constexpr unsigned meta_zoom_offset = 68;
constexpr unsigned meta_flags_offset = 28;

/* ---------------------------------------------------------------------- */
/* Console                                                                 */
/* ---------------------------------------------------------------------- */

long console = -1;

unsigned long length(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

/* One whole line per write, so the host's transport thread, which logs a
 * command's line when it reads the command, cannot split it. */
class Line final {
public:
	Line &text(const char *s)
	{
		while (*s && size_ < sizeof(buffer_) - 1)
			buffer_[size_++] = *s++;
		return *this;
	}

	Line &number(long value)
	{
		char digits[24];
		int at = 23;
		digits[at] = 0;
		u64 v = value < 0 ? -static_cast<u64>(value) : static_cast<u64>(value);
		do {
			digits[--at] = static_cast<char>('0' + v % 10);
			v /= 10;
		} while (v);
		if (value < 0)
			digits[--at] = '-';
		return text(digits + at);
	}

	void write()
	{
		text("\n");
		sys(nr_write, console, ptr(buffer_), static_cast<long>(size_));
	}

private:
	char buffer_[200];
	unsigned long size_ = 0;
};

void say(const char *text) { Line().text("CAMRESET ").text(text).write(); }

void sleep_ms(long milliseconds)
{
	const long request[2] = {milliseconds / 1000,
				 (milliseconds % 1000) * 1000000};
	sys(nr_nanosleep, ptr(request), 0);
}

long now_ms()
{
	long now[2] = {0, 0};
	sys(nr_clock_gettime, 1, ptr(now));
	return now[0] * 1000 + now[1] / 1000000;
}

bool exists(const char *path)
{
	return sys(nr_faccessat, at_fdcwd, ptr(path), 0) == 0;
}

[[noreturn]] void power_off()
{
	sys(nr_sync);
	sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc);
	for (;;)
		sys(nr_exit_group, 1);
}

[[noreturn]] void fail(const char *why)
{
	Line().text("CAMRESET FAIL ").text(why).write();
	power_off();
}

/* Writes `text` to the sysfs file at `path`; the write's result. */
long write_file(const char *path, const char *text)
{
	const long fd = sys(nr_openat, at_fdcwd, ptr(path), o_wronly);
	if (fd < 0)
		return fd;
	const long result =
		sys(nr_write, fd, ptr(text), static_cast<long>(length(text)));
	sys(nr_close, fd);
	return result;
}

/* ---------------------------------------------------------------------- */
/* A V4L2 node                                                             */
/* ---------------------------------------------------------------------- */

constexpr unsigned max_buffers = 8;

struct Node {
	long fd = -1;
	u32 type = 0;
	bool meta = false;
	unsigned count = 0;
	u8 *address[max_buffers] = {};
	unsigned long size[max_buffers] = {};
};

long ioctl(const Node &node, u32 request, void *argument)
{
	return sys(nr_ioctl, node.fd, request, ptr(argument));
}

void clear(void *memory, unsigned long bytes)
{
	auto *at = static_cast<u8 *>(memory);
	for (unsigned long i = 0; i < bytes; i++)
		at[i] = 0;
}

bool open_node(Node &node, const char *path, u32 type)
{
	node.type = type;
	node.meta = type == buf_type_meta_capture;
	node.fd = sys(nr_openat, at_fdcwd, ptr(path), o_rdwr | o_nonblock);
	return node.fd >= 0;
}

bool set_video_format(Node &node)
{
	Format format;
	clear(&format, sizeof(format));
	format.type = node.type;
	format.pix_mp.pixelformat = pix_fmt_nv12;
	format.pix_mp.width = 1280;
	format.pix_mp.height = 720;
	format.pix_mp.num_planes = 1;
	return ioctl(node, vidioc_s_fmt, &format) == 0 && format.pix_mp.width == 1280 &&
	       format.pix_mp.height == 720;
}

struct Request {
	Buffer buffer;
	Plane plane;
};

void prepare(const Node &node, Request &request, u32 index)
{
	clear(&request, sizeof(request));
	request.buffer.index = index;
	request.buffer.type = node.type;
	request.buffer.memory = memory_mmap;
	if (!node.meta) {
		request.buffer.length = 1;
		request.buffer.m.planes = &request.plane;
	}
}

bool allocate(Node &node, unsigned wanted)
{
	RequestBuffers request;
	clear(&request, sizeof(request));
	request.count = wanted;
	request.type = node.type;
	request.memory = memory_mmap;
	if (ioctl(node, vidioc_reqbufs, &request) != 0 || request.count != wanted ||
	    wanted > max_buffers)
		return false;
	node.count = wanted;
	for (unsigned index = 0; index < wanted; index++) {
		Request query;
		prepare(node, query, index);
		if (ioctl(node, vidioc_querybuf, &query.buffer) != 0)
			return false;
		const unsigned long bytes =
			node.meta ? query.buffer.length : query.plane.length;
		const long offset = node.meta ? static_cast<long>(query.buffer.m.offset) :
						static_cast<long>(query.plane.m.mem_offset);
		const long address = sys(nr_mmap, 0, static_cast<long>(bytes),
					 prot_read | prot_write, map_shared, node.fd, offset);
		if (address < 0 && address > -4096)
			return false;
		node.address[index] = reinterpret_cast<u8 *>(address);
		node.size[index] = bytes;
	}
	return true;
}

bool queue(const Node &node, unsigned index)
{
	Request request;
	prepare(node, request, index);
	return ioctl(node, vidioc_qbuf, &request.buffer) == 0;
}

struct Dequeued {
	u32 index;
	u32 flags;
	u32 sequence;
	u32 bytesused;
	long stamp_ms;
};

/* 1 for a buffer, 0 for none ready, a negative errno for a failure. */
long dequeue(const Node &node, Dequeued &out)
{
	Request request;
	prepare(node, request, 0);
	const long result = ioctl(node, vidioc_dqbuf, &request.buffer);
	if (result == -eagain)
		return 0;
	if (result < 0)
		return result;
	out.index = request.buffer.index;
	out.flags = request.buffer.flags;
	out.sequence = request.buffer.sequence;
	out.bytesused = node.meta ? request.buffer.bytesused : request.plane.bytesused;
	out.stamp_ms = request.buffer.timestamp.sec * 1000 + request.buffer.timestamp.usec / 1000;
	return 1;
}

bool stream(const Node &node, bool on)
{
	int type = static_cast<int>(node.type);
	return ioctl(node, on ? vidioc_streamon : vidioc_streamoff, &type) == 0;
}

bool set_zoom(const Node &node, u32 value)
{
	ExtControl control;
	clear(&control, sizeof(control));
	control.id = cid_zoom_q16;
	control.value = static_cast<s32>(value);
	ExtControls controls;
	clear(&controls, sizeof(controls));
	controls.which = cid_zoom_q16 & 0x0fff0000u;
	controls.count = 1;
	controls.controls = &control;
	return ioctl(node, vidioc_s_ext_ctrls, &controls) == 0;
}

/* ---------------------------------------------------------------------- */
/* What the client saw                                                     */
/* ---------------------------------------------------------------------- */

constexpr unsigned kept = 512;

struct Tally {
	unsigned frames = 0;
	unsigned frame_errors = 0;
	unsigned records = 0;
	unsigned record_errors = 0;
	/* A sequence that did not rise past the one before it. */
	unsigned frame_regressions = 0;
	unsigned record_regressions = 0;
	bool have_frame = false;
	u32 last_frame = 0;
	bool have_record = false;
	u64 last_record = 0;
	u32 last_zoom = 0;
	/* The age of the steady frames: the host stamps each one capture_latency
	 * before it delivers it, and the driver subtracts that from its clock. */
	unsigned since_start = 0;
	unsigned young = 0;
	unsigned aged = 0;
	/* What came after the resume. */
	bool resumed = false;
	unsigned after_frames = 0;
	unsigned after_records = 0;
	/* The records after the resume that show the default zoom, in one run, and
	 * whether that run is over: see took_record. */
	unsigned default_zoom_records = 0;
	bool in_default_zoom = false;
	bool zoom_recovered = false;
	unsigned after_wrong_zoom = 0;
	u32 first_after_frame = 0;
	u32 after_frame[kept] = {};
	u64 after_record[kept] = {};
};

Tally tally;
Node video;
Node metadata;

/* The host's capture latency, in milliseconds; a frame older than this when
 * the client gets it has had the age taken off its timestamp. */
constexpr long age_floor_ms = 250;
/* The first frames of a stream, or of one made again, are black stand-ins the
 * device times itself. */
constexpr unsigned stand_in_frames = 30;

void took_frame(const Dequeued &frame, long now)
{
	tally.frames++;
	if (!(frame.flags & buf_flag_error) && ++tally.since_start > stand_in_frames) {
		if (now - frame.stamp_ms >= age_floor_ms)
			tally.aged++;
		else
			tally.young++;
	}
	if (frame.flags & buf_flag_error)
		tally.frame_errors++;
	if (tally.have_frame && frame.sequence <= tally.last_frame) {
		if (tally.frame_regressions++ == 0)
			Line().text("CAMRESET frame sequence ").number(frame.sequence)
				.text(" after ").number(tally.last_frame)
				.text(" (frame ").number(tally.frames).text(")").write();
	}
	tally.have_frame = true;
	tally.last_frame = frame.sequence;
	if (tally.resumed) {
		if (tally.after_frames == 0)
			tally.first_after_frame = frame.sequence;
		if (tally.after_frames < kept)
			tally.after_frame[tally.after_frames] = frame.sequence;
		tally.after_frames++;
	}
}

void took_record(const Dequeued &record)
{
	if ((record.flags & buf_flag_error) || record.bytesused != meta_size) {
		tally.record_errors++;
		return;
	}
	const u8 *bytes = metadata.address[record.index];
	u64 sequence = 0;
	u32 zoom = 0, flags = 0;
	for (int i = 7; i >= 0; i--)
		sequence = (sequence << 8) | bytes[i];
	for (int i = 3; i >= 0; i--)
		zoom = (zoom << 8) | bytes[meta_zoom_offset + i];
	for (int i = 3; i >= 0; i--)
		flags = (flags << 8) | bytes[meta_flags_offset + i];
	tally.records++;
	if (tally.have_record && sequence <= tally.last_record) {
		/* The first one is told, with its neighbours, for the log. */
		if (tally.record_regressions++ == 0)
			Line().text("CAMRESET record sequence ").number(static_cast<long>(sequence))
				.text(" after ").number(static_cast<long>(tally.last_record))
				.text(" (record ").number(tally.records).text(")").write();
	}
	tally.have_record = true;
	tally.last_record = sequence;
	tally.last_zoom = zoom;
	if (tally.resumed) {
		/*
		 * The records the client gets first after a resume are the ones the
		 * old session made before the suspend (the device keeps its stream
		 * going while the guest is frozen, and its metadata waits in the
		 * rings), and they show the zoom. Then come the new session's: the
		 * device starts a stream's capture at its first queued buffer, and
		 * the platform makes its first exposures then, which can be before the
		 * zoom it was told of reaches it. Those records show the default
		 * zoom, truthfully, and they come at no fixed place among the
		 * records the client dequeues, so the rule is about the shape: the
		 * default zoom may show in one run of records, and once the zoom
		 * shows again it stays. Any other value, a second run, or a zoom that
		 * never returns is wrong.
		 */
		if (zoom == zoom_two) {
			if (tally.in_default_zoom) {
				tally.in_default_zoom = false;
				tally.zoom_recovered = true;
			}
		} else if (zoom == zoom_one && !tally.zoom_recovered) {
			tally.in_default_zoom = true;
			tally.default_zoom_records++;
		} else if (tally.after_wrong_zoom++ == 0) {
			Line().text("CAMRESET wrong zoom ").number(static_cast<long>(zoom))
				.text(" in record ").number(static_cast<long>(sequence))
				.text(" (record ").number(tally.after_records + 1)
				.text(" after the resume, flags ").number(static_cast<long>(flags))
				.text(tally.zoom_recovered ? ", after the zoom had returned" : "")
				.text(")").write();
		}
		if (tally.after_records < kept)
			tally.after_record[tally.after_records] = sequence;
		tally.after_records++;
	}
}

/* Takes what is ready on both nodes and queues the buffers again; false when a
 * node fails. */
bool pump_once(bool &idle)
{
	idle = true;
	Dequeued done;
	long result;
	while ((result = dequeue(video, done)) > 0) {
		idle = false;
		took_frame(done, now_ms());
		if (!queue(video, done.index))
			return false;
	}
	if (result < 0)
		return false;
	while ((result = dequeue(metadata, done)) > 0) {
		idle = false;
		took_record(done);
		if (!queue(metadata, done.index))
			return false;
	}
	return result == 0;
}

/* Pumps until `done` holds; false on a failure or when `seconds` pass. */
template <typename Done> bool pump_until(Done done, long seconds)
{
	const long deadline = now_ms() + seconds * 1000;
	while (now_ms() < deadline) {
		bool idle;
		if (!pump_once(idle))
			return false;
		if (done())
			return true;
		if (idle)
			sleep_ms(5);
	}
	return false;
}

/* After the resume: how many post-resume frames have a record of their
 * sequence. */
unsigned matched()
{
	unsigned found = 0;
	const unsigned frames = tally.after_frames < kept ? tally.after_frames : kept;
	const unsigned records = tally.after_records < kept ? tally.after_records : kept;
	for (unsigned f = 0; f < frames; f++)
		for (unsigned r = 0; r < records; r++)
			if (tally.after_record[r] == tally.after_frame[f]) {
				found++;
				break;
			}
	return found;
}

/* A buffer the client has got back is the client's: fill it, wait, read it. */
constexpr u8 pattern = 0xa5;

void fill(Node &node)
{
	for (unsigned i = 0; i < node.count; i++)
		for (unsigned long b = 0; b < node.size[i]; b++)
			node.address[i][b] = pattern;
}

unsigned long changed(Node &node)
{
	unsigned long count = 0;
	for (unsigned i = 0; i < node.count; i++)
		for (unsigned long b = 0; b < node.size[i]; b++)
			if (node.address[i][b] != pattern)
				count++;
	return count;
}

/* The host's /alarm file: how long the RTC alarm that ends the suspend runs. */
bool read_alarm(char *into, unsigned long room)
{
	const long fd = sys(nr_openat, at_fdcwd, ptr("/alarm"), o_rdonly);
	if (fd < 0)
		return false;
	const long got = sys(nr_read, fd, ptr(into), static_cast<long>(room - 1));
	sys(nr_close, fd);
	if (got <= 0)
		return false;
	into[got] = 0;
	return true;
}

/* The slow-stop case: the metadata node alone, its buffers held by the device. */
[[noreturn]] void run_slow_stop()
{
	for (int attempt = 0; attempt < 600 && !exists("/dev/video11"); ++attempt)
		sleep_ms(100);
	if (!open_node(metadata, "/dev/video11", buf_type_meta_capture))
		fail("no metadata node");
	if (!allocate(metadata, 4))
		fail("REQBUFS");
	for (unsigned i = 0; i < metadata.count; i++)
		if (!queue(metadata, i))
			fail("QBUF metadata");
	if (!stream(metadata, true))
		fail("STREAMON");
	say("holding");
	/* No video stream runs, so no record may come. */
	sleep_ms(1500);
	Dequeued record;
	if (dequeue(metadata, record) != 0)
		fail("a record came with no exposure");

	const long stop = now_ms();
	if (!stream(metadata, false))
		fail("STREAMOFF metadata");
	const long stopped = now_ms() - stop;
	fill(metadata);
	sleep_ms(3000);
	const unsigned long overwritten = changed(metadata);
	Line().text("CAMRESET streamoff ms=").number(stopped)
		.text(" overwritten=").number(static_cast<long>(overwritten)).write();
	say(overwritten == 0 ? "PASS" : "FAILED");
	power_off();
}

} // namespace

extern "C" void *memset(void *d, int c, unsigned long n)
{
	auto *b = static_cast<unsigned char *>(d);
	for (unsigned long i = 0; i < n; i++)
		b[i] = static_cast<unsigned char>(c);
	return d;
}

extern "C" void *memcpy(void *d, const void *s, unsigned long n)
{
	auto *a = static_cast<unsigned char *>(d);
	auto *b = static_cast<const unsigned char *>(s);
	for (unsigned long i = 0; i < n; i++)
		a[i] = b[i];
	return d;
}

extern "C" [[noreturn]] void _start()
{
	sys(nr_mkdirat, at_fdcwd, ptr("/dev"), 0755);
	sys(nr_mkdirat, at_fdcwd, ptr("/sys"), 0755);
	sys(nr_mount, ptr("devtmpfs"), ptr("/dev"), ptr("devtmpfs"), 0, 0);
	sys(nr_mount, ptr("sysfs"), ptr("/sys"), ptr("sysfs"), 0, 0);
	console = sys(nr_openat, at_fdcwd, ptr("/dev/console"), o_wronly);
	say("started");
	if (exists("/stop"))
		run_slow_stop();

	/* The first camera's capture and metadata nodes. */
	for (int attempt = 0; attempt < 600 && !(exists("/dev/video10") &&
						exists("/dev/video11")); ++attempt)
		sleep_ms(100);
	if (!open_node(video, "/dev/video10", buf_type_capture_mplane) ||
	    !open_node(metadata, "/dev/video11", buf_type_meta_capture))
		fail("no camera nodes");
	if (!set_video_format(video))
		fail("S_FMT");
	if (!allocate(video, 4) || !allocate(metadata, 8))
		fail("REQBUFS");
	for (unsigned i = 0; i < video.count; i++)
		if (!queue(video, i))
			fail("QBUF video");
	for (unsigned i = 0; i < metadata.count; i++)
		if (!queue(metadata, i))
			fail("QBUF metadata");
	if (!stream(video, true) || !stream(metadata, true))
		fail("STREAMON");
	say("streaming");

	if (!set_zoom(video, zoom_two))
		fail("zoom");
	if (!pump_until([] {
		    return tally.frames >= 50 && tally.records >= 5 &&
			   tally.last_zoom == zoom_two;
	    }, 90))
		fail("no frames, or no record showing the zoom, before the suspend");
	Line().text("CAMRESET before frames=").number(tally.frames)
		.text(" records=").number(tally.records)
		.text(" lastframe=").number(tally.last_frame)
		.text(" lastrecord=").number(static_cast<long>(tally.last_record)).write();

	char alarm[16];
	if (!read_alarm(alarm, sizeof(alarm)))
		fail("no /alarm");
	if (write_file("/sys/class/rtc/rtc0/wakealarm", alarm) < 0)
		fail("no alarm");
	say("suspending");
	const long suspended = write_file("/sys/power/state", "mem");
	tally.resumed = true;
	tally.since_start = 0;
	Line().text("CAMRESET resumed result=").number(suspended).write();

	/* The stream carries on: frames with their records, the zoom in them. */
	if (!pump_until([] {
		    return tally.after_frames >= 60 && tally.after_records >= 10 &&
			   !tally.in_default_zoom;
	    }, 120))
		fail("the stream did not carry on after the suspend, or its zoom did not return");
	const unsigned found = matched();
	Line().text("CAMRESET after frames=").number(tally.after_frames)
		.text(" records=").number(tally.after_records)
		.text(" errors=").number(tally.frame_errors)
		.text(" firstframe=").number(tally.first_after_frame)
		.text(" matched=").number(found)
		.text(" defaultzoomrecords=").number(tally.default_zoom_records)
		.text(" wrongzoom=").number(tally.after_wrong_zoom)
		.text(" frameregressions=").number(tally.frame_regressions)
		.text(" recordregressions=").number(tally.record_regressions)
		.text(" recorderrors=").number(tally.record_errors).write();
	Line().text("CAMRESET first after: frames ").number(tally.after_frame[0]).text(" ")
		.number(tally.after_frame[1]).text(" ").number(tally.after_frame[2])
		.text(", records ").number(static_cast<long>(tally.after_record[0])).text(" ")
		.number(static_cast<long>(tally.after_record[1])).text(" ")
		.number(static_cast<long>(tally.after_record[2])).write();

	Line().text("CAMRESET ages young=").number(tally.young)
		.text(" aged=").number(tally.aged).write();

	/* The client stops the stream and owns its buffers again; nothing may
	 * write them any more. The host delays the device's work in one case, so
	 * the time STREAMOFF takes is told too. */
	const long stop = now_ms();
	if (!stream(video, false))
		fail("STREAMOFF video");
	const long stopped = now_ms() - stop;
	fill(video);
	if (!stream(metadata, false))
		fail("STREAMOFF metadata");
	sleep_ms(3000);
	const unsigned long overwritten = changed(video);
	Line().text("CAMRESET streamoff ms=").number(stopped)
		.text(" overwritten=").number(static_cast<long>(overwritten)).write();

	const bool good = tally.young == 0 && tally.aged >= 20 && tally.frame_errors == 0 && tally.frame_regressions == 0 &&
			  tally.record_regressions == 0 && tally.record_errors == 0 &&
			  tally.after_wrong_zoom == 0 && tally.last_zoom == zoom_two &&
			  found >= 3 && overwritten == 0;
	say(good ? "PASS" : "FAILED");
	power_off();
}
