// SPDX-License-Identifier: GPL-2.0-or-later
// Scripts the host's answers to the UBD driver's held-request functions
// (test_darwin_ubd_hold.py builds them from the series) and checks what the
// driver does with each: which failures it holds, how long it waits, what it
// redoes and what it prints.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include "kernel.h"
void test_run(struct ubd *, struct io_thread_req *, char *, unsigned long,
	      unsigned long);
void test_set_stopped(bool);
unsigned long jiffies = 1000;
}

namespace {

struct Attempt {
	std::string what;
	u64 entry_offset;
	unsigned long length;
	unsigned long byte_offset;
};

std::vector<Attempt> attempts;
std::deque<int> script;  // the status of each attempt; an empty script is OK
bool advance_on_failure = false;
std::vector<unsigned long> waits;
std::vector<std::string> lines;
std::function<void(size_t)> on_wait;
int failures = 0;

void check(bool condition, const char *what, int line)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
		failures++;
	}
}
#define CHECK(condition) check((condition), #condition, __LINE__)

void attempt(const char *what, struct io_thread_req *request,
	     unsigned long length, unsigned long byte_offset)
{
	attempts.push_back({what, request->offset, length, byte_offset});
	int status = BLK_STS_OK;
	if (!script.empty()) {
		status = script.front();
		script.pop_front();
	}
	request->error = (blk_status_t)status;
	if (status == BLK_STS_OK || advance_on_failure)
		request->offset += length;
}

struct Fixture {
	struct request request;
	struct request_queue queue;
	struct io_thread_req io;
	struct gendisk disk;
	struct gendisk unassigned;  // what dev->disk names while a disk is added
	struct ubd dev;

	Fixture(enum req_op op, bool direct = true, u64 offset = 4096 * 7)
	{
		attempts.clear();
		script.clear();
		waits.clear();
		lines.clear();
		on_wait = nullptr;
		advance_on_failure = false;
		jiffies = 1000;
		test_set_stopped(false);
		request.op = op;
		request.q = &queue;
		queue.disk = &disk;
		io = {&request, direct, offset, BLK_STS_OK};
		std::strcpy(disk.disk_name, "ubdb");
		// The driver may run a request before it assigns dev->disk, so the
		// lines must name the disk through the request's queue.
		std::strcpy(unassigned.disk_name, "not the request's disk");
		dev = {&unassigned};
	}
	void run(unsigned long length = 4096, unsigned long byte_offset = 4096 * 7)
	{
		static char buffer[4096];
		test_run(&dev, &io, buffer, length, byte_offset);
	}
};

}  // namespace

extern "C" {

void darwin_ubd_do_segment(struct ubd *, struct io_thread_req *io_req,
			   char *, unsigned long length,
			   unsigned long byte_offset)
{
	attempt("segment", io_req, length, byte_offset);
}

void do_io(struct io_thread_req *req, struct io_desc *desc)
{
	CHECK(desc == nullptr);
	attempt("flush", req, 0, 0);
}

void test_print(const char *level, const char *format, ...)
{
	char text[256];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	std::string line = text;
	CHECK(!line.empty() && line.back() == '\n');
	if (!line.empty())
		line.pop_back();
	lines.push_back(std::string(level) + ": " + line);
}

void test_wait(unsigned long timeout)
{
	if (waits.size() >= 100) {
		std::fputs("FAIL: a hold that never ends\n", stderr);
		std::exit(1);
	}
	waits.push_back(timeout);
	jiffies += timeout;
	if (on_wait)
		on_wait(waits.size());
}

}  // extern "C"

static bool is(const std::vector<unsigned long> &actual,
	       std::initializer_list<unsigned long> expected)
{
	return actual == std::vector<unsigned long>(expected);
}

int main()
{
	// An ordinary write is one attempt, no wait and no line.
	{
		Fixture f(REQ_OP_WRITE);
		f.run();
		CHECK(attempts.size() == 1 && waits.empty() && lines.empty());
		CHECK(f.io.error == BLK_STS_OK && f.io.offset == 4096 * 8);
	}

	// A write refused for want of space is redone, from the same offset and
	// with the same buffer window, after waits that double from 10 ms. The
	// driver says so once when the hold begins and once when it ends.
	{
		Fixture f(REQ_OP_WRITE);
		script = {BLK_STS_NOSPC, BLK_STS_NOSPC, BLK_STS_NOSPC};
		f.run();
		CHECK(attempts.size() == 4);
		for (const Attempt &a : attempts)
			CHECK(a.what == "segment" && a.entry_offset == 4096 * 7 &&
			      a.length == 4096 && a.byte_offset == 4096 * 7);
		CHECK(is(waits, {10, 20, 40}));
		CHECK(f.io.error == BLK_STS_OK && f.io.offset == 4096 * 8);
		CHECK(lines.size() == 2);
		CHECK(lines.size() == 2 &&
		      lines[0] == "warn: ubdb: host storage is full; holding I/O "
				  "until it has room");
		CHECK(lines.size() == 2 &&
		      lines[1] == "info: ubdb: host storage has room again after "
				  "70 ms; I/O resumed");
	}

	// The wait stops growing at one second, and a long hold is still two lines.
	{
		Fixture f(REQ_OP_WRITE);
		for (int i = 0; i < 12; i++)
			script.push_back(BLK_STS_NOSPC);
		f.run();
		CHECK(is(waits, {10, 20, 40, 80, 160, 320, 640, 1000, 1000, 1000,
				 1000, 1000}));
		CHECK(attempts.size() == 13 && lines.size() == 2);
		CHECK(f.io.error == BLK_STS_OK);
	}

	// A write that advances the offset before it fails, as a COW update of the
	// bitmap does, is redone from the offset it started at.
	{
		Fixture f(REQ_OP_WRITE);
		advance_on_failure = true;
		script = {BLK_STS_NOSPC, BLK_STS_NOSPC};
		f.run();
		CHECK(attempts.size() == 3);
		for (const Attempt &a : attempts)
			CHECK(a.entry_offset == 4096 * 7);
		CHECK(f.io.offset == 4096 * 8 && f.io.error == BLK_STS_OK);
	}

	// A flush and a write of zeroes are held like a write.
	{
		Fixture f(REQ_OP_FLUSH);
		script = {BLK_STS_NOSPC};
		f.run(0, 0);
		CHECK(attempts.size() == 2 && attempts[0].what == "flush" &&
		      attempts[1].what == "flush" && is(waits, {10}));
		CHECK(f.io.error == BLK_STS_OK && lines.size() == 2);
	}
	{
		Fixture f(REQ_OP_WRITE_ZEROES);
		script = {BLK_STS_NOSPC, BLK_STS_NOSPC};
		f.run(1 << 20, 4096 * 7);
		CHECK(attempts.size() == 3 && is(waits, {10, 20}));
		CHECK(f.io.error == BLK_STS_OK);
	}

	// A discard is never held: it fails as it always did, silently.
	{
		Fixture f(REQ_OP_DISCARD);
		script = {BLK_STS_NOSPC};
		f.run(1 << 20, 4096 * 7);
		CHECK(attempts.size() == 1 && waits.empty() && lines.empty());
		CHECK(f.io.error == BLK_STS_NOSPC);
	}

	// A COW disk is never held.
	{
		Fixture f(REQ_OP_WRITE, false);
		script = {BLK_STS_NOSPC};
		f.run();
		CHECK(attempts.size() == 1 && waits.empty() && lines.empty());
		CHECK(f.io.error == BLK_STS_NOSPC);
	}

	// Every other failure ends the request at once, as before.
	for (int status : {BLK_STS_IOERR, BLK_STS_NOTSUPP}) {
		Fixture f(REQ_OP_WRITE);
		script = {status};
		f.run();
		CHECK(attempts.size() == 1 && waits.empty() && lines.empty());
		CHECK(f.io.error == status);
	}

	// Once the exitcall has begun, a refusal is not held.
	{
		Fixture f(REQ_OP_WRITE);
		test_set_stopped(true);
		script = {BLK_STS_NOSPC};
		f.run();
		CHECK(attempts.size() == 1 && waits.empty() && lines.empty());
		CHECK(f.io.error == BLK_STS_NOSPC);
	}

	// A hold that the exitcall ends stops waiting, tries once more, and then
	// fails the request with the status it has. It never says the space is
	// back.
	{
		Fixture f(REQ_OP_WRITE);
		for (int i = 0; i < 20; i++)
			script.push_back(BLK_STS_NOSPC);
		on_wait = [](size_t n) {
			if (n == 2)
				test_set_stopped(true);
		};
		f.run();
		CHECK(is(waits, {10, 20}));
		CHECK(attempts.size() == 3 && f.io.error == BLK_STS_NOSPC);
		CHECK(lines.size() == 1 && lines[0].rfind("warn: ", 0) == 0);
	}

	// A hold ends in another failure without claiming the space came back.
	{
		Fixture f(REQ_OP_WRITE);
		script = {BLK_STS_NOSPC, BLK_STS_IOERR};
		f.run();
		CHECK(attempts.size() == 2 && f.io.error == BLK_STS_IOERR);
		CHECK(lines.size() == 1);
	}

	if (failures) {
		std::fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	std::puts("PASS UBD hold: redo from the failed segment, back-off, one line per "
		  "hold, no hold for discard, COW, other errors or a stopped driver");
	return 0;
}
