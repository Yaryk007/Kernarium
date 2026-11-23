// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 Linux guest probe: what the kernel's flush after a write
 * into a mapped page costs, and breaks, for memory that holds no code.
 *
 * The x86_64 kernel names to the host every page-cache or memfd frame it
 * wrote that a mapping with execute permission was installed for, so that a
 * translator holding it hears of the write. A frame mapped without execute
 * holds no code, so nothing about it is named, and the kernel's own writers of
 * user-mapped memory, which run in interrupt context where the host cannot be
 * called, must be left alone. One mode runs per boot:
 *
 *	ring1, ring2, ring3	an AF_PACKET receive ring (TPACKET_V1, V2 and V3)
 *				is mapped readable and writable and read, and
 *				packets on the loopback interface arrive in it
 *				from the network softirq, with panic_on_warn
 *				set: the kernel must not warn;
 *	marks			the kernel marks the frame of a page whose
 *				mapping is executable (mapped so, or made so by
 *				mprotect() after the page was present) and of
 *				no other: not an anonymous page, a file page
 *				mapped readable and writable, a packet ring or
 *				an AIO ring, which interrupt handlers write;
 *	data			files mapped readable, or readable and writable, and
 *				not executable are written with write(2) and
 *				pwrite(2), by the process that maps them and by
 *				another, and read: the host must be named no
 *				frame (the runner reads the emulator's count);
 *	code			the control of "data": a file mapped executable and
 *				run is written the same way, and the host is
 *				named its frames.
 *
 * Prints "page-cache-flush: mode=... " and a verdict. */
namespace {

using word = unsigned long;

constexpr long nr_write = 1, nr_open = 2, nr_close = 3, nr_mmap = 9,
	nr_ioctl = 16, nr_pread64 = 17, nr_pwrite64 = 18, nr_nanosleep = 35,
	nr_socket = 41, nr_sendto = 44, nr_bind = 49, nr_setsockopt = 54, nr_fork = 57,
	nr_wait4 = 61, nr_unlink = 87, nr_ftruncate = 77, nr_io_setup = 206,
	nr_exit_group = 231, nr_memfd_create = 319, nr_mprotect = 10;
constexpr long prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr long o_rdonly = 0;
constexpr long map_shared = 0x01, map_private = 0x02, map_anonymous = 0x20;
constexpr long o_wronly = 1, o_rdwr = 2, o_creat = 0100, o_excl = 0200;
constexpr word page = 4096;

long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
	long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long r10 __asm__("r10") = a3;
	register long r8 __asm__("r8") = a4;
	register long r9 __asm__("r9") = a5;
	long result;
	__asm__ volatile("syscall"
		: "=a"(result)
		: "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory", "cc");
	return result;
}

long address_of(const void *pointer)
{
	return reinterpret_cast<long>(pointer);
}

void print(const char *text)
{
	long length = 0;
	while (text[length] != 0)
		++length;
	raw_syscall(nr_write, 1, address_of(text), length);
}

void print(long value)
{
	char digits[24];
	int position = sizeof(digits);
	digits[--position] = 0;
	unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value) :
		static_cast<unsigned long>(value);
	do {
		digits[--position] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude != 0);
	if (value < 0)
		digits[--position] = '-';
	print(digits + position);
}

bool same(const char *left, const char *right)
{
	while (*left != 0 && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

const char *mode_name;
int failures;

void fail(const char *what, long detail = 0)
{
	++failures;
	print("page-cache-flush: FAIL mode="); print(mode_name);
	print(" "); print(what);
	print(" "); print(detail); print("\n");
}

void sleep_milliseconds(long milliseconds)
{
	long spec[2] = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
	raw_syscall(nr_nanosleep, address_of(spec), 0);
}

unsigned char *map(long length, long protection, long flags, long fd = -1)
{
	const long result = raw_syscall(nr_mmap, 0, length, protection, flags, fd, 0);
	if (result < 0 && result > -4096) {
		fail("mmap failed", result);
		return nullptr;
	}
	return reinterpret_cast<unsigned char *>(result);
}

/* Every page of a mapping is read, so that the guest's page table holds it
 * and the host has been given it. */
unsigned touch(const unsigned char *region, word length)
{
	unsigned sum = 0;
	for (word at = 0; at < length; at += page)
		sum += static_cast<const volatile unsigned char *>(region)[at];
	return sum;
}

/* The next write that warns takes the guest down. */
void panic_on_warn()
{
	const long fd = raw_syscall(nr_open, address_of("/proc/sys/kernel/panic_on_warn"), o_wronly);
	if (fd < 0 || raw_syscall(nr_write, fd, address_of("1"), 1) != 1)
		fail("panic_on_warn is not settable", fd);
	raw_syscall(nr_close, fd);
}

struct SockaddrIn final {
	unsigned short family;
	unsigned short port;
	unsigned int address;
	unsigned char zero[8];
};

struct SockaddrLl final {
	unsigned short family;
	unsigned short protocol;
	int ifindex;
	unsigned short hatype;
	unsigned char pkttype;
	unsigned char halen;
	unsigned char address[8];
};

struct IfReq final {
	char name[16];
	union {
		short flags;
		int index;
		char pad[24];
	} value;
};

struct TpacketReq3 final {
	unsigned int block_size;
	unsigned int block_nr;
	unsigned int frame_size;
	unsigned int frame_nr;
	unsigned int retire_blk_tov;
	unsigned int sizeof_priv;
	unsigned int feature_req_word;
};

constexpr long af_inet = 2, af_packet = 17, sock_dgram = 2, sock_raw = 3;
constexpr long sol_packet = 263, packet_version = 10, packet_rx_ring = 5;
constexpr long siocgifflags = 0x8913, siocsifflags = 0x8914, siocgifindex = 0x8933;
constexpr unsigned short eth_p_all_network_order = 0x0300;
constexpr word ring_block = page, ring_blocks = 4, ring_frame = 2048;

/* Brings the loopback interface up, which gives it 127.0.0.1, and returns a
 * datagram socket and the interface's index. */
bool loopback(long &udp, int &index)
{
	udp = raw_syscall(nr_socket, af_inet, sock_dgram, 0);
	if (udp < 0) {
		fail("no datagram socket", udp);
		return false;
	}
	IfReq request{};
	request.name[0] = 'l';
	request.name[1] = 'o';
	if (raw_syscall(nr_ioctl, udp, siocgifflags, address_of(&request)) != 0) {
		fail("no loopback interface");
		return false;
	}
	request.value.flags = static_cast<short>(request.value.flags | 1);
	if (raw_syscall(nr_ioctl, udp, siocsifflags, address_of(&request)) != 0) {
		fail("the loopback interface did not come up");
		return false;
	}
	request.value.index = 0;
	if (raw_syscall(nr_ioctl, udp, siocgifindex, address_of(&request)) != 0) {
		fail("no loopback interface index");
		return false;
	}
	index = request.value.index;
	return true;
}

/* A receive ring of the given TPACKET version, read, with packets arriving in
 * it from the loopback interface. */
void case_packet_ring(int version)
{
	long udp = -1;
	int index = 0;
	if (!loopback(udp, index))
		return;
	panic_on_warn();
	const long packet = raw_syscall(nr_socket, af_packet, sock_raw, eth_p_all_network_order);
	if (packet < 0) {
		fail("no packet socket", packet);
		return;
	}
	int chosen = version;
	if (raw_syscall(nr_setsockopt, packet, sol_packet, packet_version, address_of(&chosen),
			sizeof(chosen)) != 0) {
		fail("no such TPACKET version", version);
		return;
	}
	TpacketReq3 request{};
	request.block_size = ring_block;
	request.block_nr = ring_blocks;
	request.frame_size = ring_frame;
	request.frame_nr = ring_blocks * (ring_block / ring_frame);
	request.retire_blk_tov = 10;
	if (raw_syscall(nr_setsockopt, packet, sol_packet, packet_rx_ring, address_of(&request),
			version == 2 ? sizeof(request) : 16) != 0) {
		fail("no receive ring");
		return;
	}
	auto *ring = map(ring_block * ring_blocks, prot_read | prot_write, map_shared, packet);
	if (ring == nullptr)
		return;
	SockaddrLl bound{};
	bound.family = af_packet;
	bound.protocol = eth_p_all_network_order;
	bound.ifindex = index;
	if (raw_syscall(nr_bind, packet, address_of(&bound), sizeof(bound)) != 0) {
		fail("the ring did not bind to the loopback interface");
		return;
	}
	touch(ring, ring_block * ring_blocks);

	SockaddrIn peer{};
	peer.family = af_inet;
	peer.port = 0x0900;
	peer.address = 0x0100007f;
	unsigned char payload[1400] = {};
	for (int round = 0; round < 20; ++round) {
		const long sent = raw_syscall(nr_sendto, udp, address_of(payload), sizeof(payload), 0,
			address_of(&peer), sizeof(peer));
		if (sent != static_cast<long>(sizeof(payload))) {
			fail("the datagram was not sent", sent);
			return;
		}
	}

	/* A filled V1 or V2 frame, or a retired V3 block, is the user's. */
	sleep_milliseconds(100);
	long filled = 0;
	for (int wait = 0; wait < 300 && filled == 0; ++wait) {
		if (version == 2) {
			for (word block = 0; block < ring_blocks; ++block)
				filled += static_cast<const volatile unsigned int *>(
					static_cast<const void *>(ring + block * ring_block + 8))[0] & 1;
		} else {
			for (word frame = 0; frame < ring_blocks * (ring_block / ring_frame); ++frame)
				filled += static_cast<const volatile unsigned int *>(
					static_cast<const void *>(ring + frame * ring_frame))[0] & 1;
		}
		if (filled == 0)
			sleep_milliseconds(10);
	}
	if (filled == 0)
		fail("no packet reached the ring");
	else {
		print("page-cache-flush: mode="); print(mode_name);
		print(" filled="); print(filled); print("\n");
	}
}

/* Whether the kernel has marked the frame behind a mapped page as one a
 * guest-code mapping was queued for: /proc/self/pagemap names the frame and
 * /proc/kpageflags carries the architecture's bit (KPF_ARCH, PG_arch_1). */
long marked(const void *address)
{
	const long pagemap = raw_syscall(nr_open, address_of("/proc/self/pagemap"), o_rdonly);
	const long flags = raw_syscall(nr_open, address_of("/proc/kpageflags"), o_rdonly);
	unsigned long long entry = 0;
	unsigned long long bits = 0;
	long result = -1;
	const long at = static_cast<long>(reinterpret_cast<word>(address) / page * 8);
	if (pagemap >= 0 && flags >= 0 &&
	    raw_syscall(nr_pread64, pagemap, address_of(&entry), 8, at) == 8 &&
	    (entry >> 63) != 0) {
		const long frame = static_cast<long>(entry & ((1ULL << 55) - 1));
		if (raw_syscall(nr_pread64, flags, address_of(&bits), 8, frame * 8) == 8)
			result = static_cast<long>((bits >> 38) & 1);
	}
	raw_syscall(nr_close, pagemap);
	raw_syscall(nr_close, flags);
	return result;
}

void expect_marked(const char *what, const void *address, long expected)
{
	const long got = marked(address);
	print("page-cache-flush: mark "); print(what); print("="); print(got); print("\n");
	if (got != expected)
		fail(what, got);
}

constexpr word file_pages = 8;
constexpr int file_writes = 100;

long memfd()
{
	const long fd = raw_syscall(nr_memfd_create, address_of("page-cache-flush"), 0);
	if (fd < 0)
		fail("memfd_create failed", fd);
	return fd;
}

long tmpfs_file()
{
	const char *path = "/tmp/.page-cache-flush";
	raw_syscall(nr_unlink, address_of(path));
	const long fd = raw_syscall(nr_open, address_of(path), o_rdwr | o_creat | o_excl, 0700);
	raw_syscall(nr_unlink, address_of(path));
	if (fd < 0)
		fail("open failed", fd);
	return fd;
}

/* Writes and reads the file the way a program does: write(2), pwrite(2) and
 * read(2) of its pages. */
void exercise(long fd)
{
	unsigned char bytes[page];
	/* Every byte is a ret, so that the file stays code a process may run. */
	for (word index = 0; index < page; ++index)
		bytes[index] = 0xc3;
	for (int round = 0; round < file_writes; ++round) {
		const long offset = static_cast<long>((round % file_pages) * page);
		if (raw_syscall(nr_pwrite64, fd, address_of(bytes), page, offset) != static_cast<long>(page))
			fail("pwrite failed", round);
		if (raw_syscall(nr_pread64, fd, address_of(bytes), page, offset) != static_cast<long>(page))
			fail("pread failed", round);
	}
}

/* Another process writes the file while this one holds the mappings. */
void exercise_in_child(long fd)
{
	const long child = raw_syscall(nr_fork);
	if (child < 0) {
		fail("fork failed", child);
		return;
	}
	if (child == 0) {
		exercise(fd);
		raw_syscall(nr_exit_group, failures == 0 ? 0 : 1);
	}
	int status = 0;
	raw_syscall(nr_wait4, child, address_of(&status), 0, 0);
	if (status != 0)
		fail("the writer failed", status);
}

/* A file nobody executes: mapped readable, and readable and writable, written
 * and read by this process and by another. */
void case_data()
{
	for (int kind = 0; kind < 2; ++kind) {
		const long fd = kind == 0 ? memfd() : tmpfs_file();
		if (fd < 0)
			return;
		raw_syscall(nr_ftruncate, fd, file_pages * page);
		auto *shared = map(file_pages * page, prot_read | prot_write, map_shared, fd);
		auto *readonly = map(file_pages * page, prot_read, map_shared, fd);
		if (shared == nullptr || readonly == nullptr)
			return;
		touch(shared, file_pages * page);
		touch(readonly, file_pages * page);
		exercise(fd);
		exercise_in_child(fd);
		touch(readonly, file_pages * page);
	}
	print("page-cache-flush: mode="); print(mode_name); print(" files=2\n");
}

/* Which mappings the kernel marks: those whose VMA is executable, however
 * they got to be (mapped so, or made so by mprotect() after the page was
 * present), and no others. */
void case_marks()
{
	const long fd = memfd();
	if (fd < 0) {
		fail("memfd_create failed");
		return;
	}
	raw_syscall(nr_ftruncate, fd, 4 * page);
	unsigned char ret = 0xc3;
	for (long number = 0; number < 4; ++number)
		raw_syscall(nr_pwrite64, fd, address_of(&ret), 1, number * static_cast<long>(page));
	auto *anonymous = map(page, prot_read | prot_write, map_private | map_anonymous);
	auto *data = map(4 * page, prot_read | prot_write, map_shared, fd);
	auto *code = map(4 * page, prot_read | prot_exec, map_shared, fd);
	if (anonymous == nullptr || data == nullptr || code == nullptr) {
		fail("mmap failed");
		return;
	}
	/* A fresh file for the mapping mprotect() makes executable: its page is
	 * present before it is. */
	const long other = memfd();
	if (other < 0) {
		fail("memfd_create failed");
		return;
	}
	raw_syscall(nr_ftruncate, other, page);
	raw_syscall(nr_pwrite64, other, address_of(&ret), 1, 0);
	auto *later = map(page, prot_read | prot_write, map_shared, other);
	if (later == nullptr) {
		fail("mmap failed");
		return;
	}
	/* Written, so that the page is its own and not the shared zero page, which
	 * every process maps and one of them may run. */
	anonymous[0] = 1;
	touch(later, page);
	expect_marked("anonymous", anonymous, 0);
	expect_marked("file-readable-writable", later, 0);
	/* Frames 0 to 3 of the first file are mapped both ways. */
	touch(data, 4 * page);
	for (long number = 0; number < 4; ++number)
		reinterpret_cast<void (*)()>(code + number * page)();
	expect_marked("file-executable", code, 1);
	expect_marked("file-executable-through-data-mapping", data, 1);
	if (raw_syscall(nr_mprotect, address_of(later), page, prot_read | prot_exec) != 0)
		fail("mprotect failed");
	reinterpret_cast<void (*)()>(later)();
	expect_marked("file-after-mprotect-to-executable", later, 1);
	/* A packet ring, which no one executes, however often it is read. */
	long udp = -1;
	int index = 0;
	if (loopback(udp, index)) { /* fails the row itself when it cannot */
		const long packet = raw_syscall(nr_socket, af_packet, sock_raw, eth_p_all_network_order);
		int chosen = 1;
		TpacketReq3 request{};
		request.block_size = ring_block;
		request.block_nr = ring_blocks;
		request.frame_size = ring_frame;
		request.frame_nr = ring_blocks * (ring_block / ring_frame);
		if (packet >= 0 &&
		    raw_syscall(nr_setsockopt, packet, sol_packet, packet_version, address_of(&chosen),
				sizeof(chosen)) == 0 &&
		    raw_syscall(nr_setsockopt, packet, sol_packet, packet_rx_ring, address_of(&request),
				16) == 0) {
			auto *ring = map(ring_block * ring_blocks, prot_read | prot_write, map_shared, packet);
			if (ring != nullptr) {
				touch(ring, ring_block * ring_blocks);
				expect_marked("packet-ring", ring, 0);
			} else {
				fail("mmap of the packet ring failed");
			}
		} else {
			fail("the packet ring could not be set up");
		}
	}
	/* The AIO ring, which no one executes either. */
	word context = 0;
	if (raw_syscall(nr_io_setup, 8, address_of(&context)) == 0) {
		touch(reinterpret_cast<const unsigned char *>(context), page);
		expect_marked("aio-ring", reinterpret_cast<const void *>(context), 0);
	} else {
		fail("io_setup failed");
	}
}

/* The same writes into a file this process runs. */
void case_code()
{
	const long fd = memfd();
	if (fd < 0)
		return;
	raw_syscall(nr_ftruncate, fd, file_pages * page);
	unsigned char ret = 0xc3;
	raw_syscall(nr_pwrite64, fd, address_of(&ret), 1, 0);
	auto *code = map(file_pages * page, prot_read | prot_exec, map_shared, fd);
	if (code == nullptr)
		return;
	reinterpret_cast<void (*)()>(code)();
	exercise(fd);
	exercise_in_child(fd);
	reinterpret_cast<void (*)()>(code)();
	print("page-cache-flush: mode="); print(mode_name); print(" files=1\n");
}

} // namespace

extern "C" {

void *memset(void *destination, int value, word count)
{
	auto *bytes = static_cast<unsigned char *>(destination);
	for (word index = 0; index < count; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

void *memcpy(void *destination, const void *source, word count)
{
	auto *to = static_cast<unsigned char *>(destination);
	const auto *from = static_cast<const unsigned char *>(source);
	for (word index = 0; index < count; ++index)
		to[index] = from[index];
	return destination;
}

int probe_main(long *stack);

__asm__(".text\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n");

int probe_main(long *stack)
{
	const long argc = stack[0];
	mode_name = argc > 1 ? reinterpret_cast<const char *>(stack[2]) : "";
	if (same(mode_name, "ring1"))
		case_packet_ring(0);
	else if (same(mode_name, "ring2"))
		case_packet_ring(1);
	else if (same(mode_name, "ring3"))
		case_packet_ring(2);
	else if (same(mode_name, "marks"))
		case_marks();
	else if (same(mode_name, "data"))
		case_data();
	else if (same(mode_name, "code"))
		case_code();
	else {
		print("page-cache-flush: unknown mode\n");
		return 4;
	}
	print("page-cache-flush: mode="); print(mode_name);
	print(" failed="); print(failures);
	print(" verdict="); print(failures == 0 ? "ok" : "fail"); print("\n");
	return failures == 0 ? 0 : 2;
}

}
