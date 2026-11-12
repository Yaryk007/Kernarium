// SPDX-License-Identifier: GPL-2.0-or-later
/* A system call never copies through a mapped host-visible Virtio-GPU blob.
 *
 * A mappable host3d blob is mapped into its process with io_remap_pfn_range(),
 * so its pages are frames of the vhost-user shared-memory aperture, outside
 * guest RAM: they have no struct page, and the kernel reaches user memory only
 * through the alias of a RAM page. A write() from such a buffer used to take a
 * reference on memory past mem_map and fault on the alias, and the guest
 * panicked with "Kernel mode signal 10". The probe maps an anonymous host3d
 * blob of the first capset the host offers, preferring Venus, and requires
 * that the process itself reads and writes it, that write() from it and
 * read() into it fail with EFAULT and leave it unchanged, and that the same
 * system calls on an ordinary buffer still work. The runner then starts one
 * more command, so a guest the copy broke fails the run.
 */

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

struct GetParam {
	uint64_t param;
	uint64_t value;
};

struct ContextParam {
	uint64_t param;
	uint64_t value;
};

struct ContextInit {
	uint32_t num_params;
	uint32_t pad;
	uint64_t ctx_set_params;
};

struct CreateBlob {
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint32_t bo_handle;
	uint32_t res_handle;
	uint64_t size;
	uint32_t pad;
	uint32_t cmd_size;
	uint64_t cmd;
	uint64_t blob_id;
};

struct Map {
	uint64_t offset;
	uint32_t handle;
	uint32_t pad;
};

constexpr unsigned command_base = 0x40;
constexpr unsigned long ioctl_map = _IOWR('d', command_base + 0x01, Map);
constexpr unsigned long ioctl_get_param = _IOWR('d', command_base + 0x03, GetParam);
constexpr unsigned long ioctl_create_blob = _IOWR('d', command_base + 0x0a, CreateBlob);
constexpr unsigned long ioctl_context_init = _IOWR('d', command_base + 0x0b, ContextInit);

constexpr uint64_t param_supported_capsets = 7;
constexpr uint64_t context_param_capset_id = 1;
constexpr uint32_t blob_mem_host3d = 2;
constexpr uint32_t blob_flag_mappable = 1;
constexpr size_t blob_size = 65536;
constexpr size_t page = 4096;

int failures = 0;

__attribute__((format(printf, 3, 4)))
void checkf(bool ok, const char *what, const char *format, ...)
{
	char detail[128];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(detail, sizeof detail, format, arguments);
	va_end(arguments);
	std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", what, detail);
	std::fflush(stdout);
	if (!ok)
		++failures;
}

int open_render_node()
{
	for (int attempt = 0; attempt < 120; ++attempt) {
		const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		sleep(1);
	}
	return -1;
}

bool all(const unsigned char *bytes, size_t length, unsigned char value)
{
	for (size_t i = 0; i < length; ++i)
		if (bytes[i] != value)
			return false;
	return true;
}

} // namespace

int main()
{
	const int fd = open_render_node();
	checkf(fd >= 0, "open the render node", "errno %d", fd >= 0 ? 0 : errno);
	if (fd < 0)
		return failures;

	uint64_t capsets = 0;
	GetParam query{param_supported_capsets, reinterpret_cast<uintptr_t>(&capsets)};
	checkf(ioctl(fd, ioctl_get_param, &query) == 0 && capsets != 0, "the device lists capsets",
	       "0x%llx", static_cast<unsigned long long>(capsets));
	uint64_t capset = 0;
	for (uint64_t id : {4u, 2u, 1u}) {
		if (capsets & (1ull << id)) {
			capset = id;
			break;
		}
	}
	ContextParam parameter{context_param_capset_id, capset};
	ContextInit context{1, 0, reinterpret_cast<uintptr_t>(&parameter)};
	checkf(capset != 0 && ioctl(fd, ioctl_context_init, &context) == 0, "create a context",
	       "capset %llu, errno %d", static_cast<unsigned long long>(capset), errno);

	CreateBlob blob{};
	blob.blob_mem = blob_mem_host3d;
	blob.blob_flags = blob_flag_mappable;
	blob.size = blob_size;
	const bool created = ioctl(fd, ioctl_create_blob, &blob) == 0 && blob.bo_handle != 0;
	checkf(created, "a mappable host3d blob", "handle %u, errno %d", blob.bo_handle,
	       created ? 0 : errno);
	Map map{0, blob.bo_handle, 0};
	const bool offset = created && ioctl(fd, ioctl_map, &map) == 0;
	checkf(offset, "the blob's map offset", "errno %d", offset ? 0 : errno);
	if (failures)
		return failures;
	auto *mapped = static_cast<unsigned char *>(
		mmap(nullptr, blob_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset));
	checkf(mapped != MAP_FAILED, "map the blob", "errno %d", mapped == MAP_FAILED ? errno : 0);
	if (mapped == MAP_FAILED)
		return failures;
	std::memset(mapped, 0x5a, blob_size);
	checkf(all(mapped, blob_size, 0x5a), "the process reads what it wrote to the blob", "%s",
	       "");

	const int file = open("/tmp/blob-uaccess", O_RDWR | O_CREAT | O_TRUNC, 0600);
	checkf(file >= 0, "open a file", "errno %d", file >= 0 ? 0 : errno);
	if (file < 0)
		return failures;

	errno = 0;
	ssize_t result = write(file, mapped + 256, page);
	checkf(result == -1 && errno == EFAULT, "write() from the blob", "returned %zd, errno %d",
	       result, result < 0 ? errno : 0);

	unsigned char ordinary[page];
	std::memset(ordinary, 0xa5, sizeof ordinary);
	result = pwrite(file, ordinary, sizeof ordinary, 0);
	checkf(result == static_cast<ssize_t>(page), "write() from an ordinary buffer",
	       "returned %zd, errno %d", result, result < 0 ? errno : 0);
	errno = 0;
	result = pread(file, mapped + 256, page, 0);
	checkf(result == -1 && errno == EFAULT, "read() into the blob", "returned %zd, errno %d",
	       result, result < 0 ? errno : 0);
	checkf(all(mapped, blob_size, 0x5a), "the blob is unchanged", "%s", "");
	std::memset(ordinary, 0, sizeof ordinary);
	result = pread(file, ordinary, sizeof ordinary, 0);
	checkf(result == static_cast<ssize_t>(page) && all(ordinary, page, 0xa5),
	       "read() into an ordinary buffer", "returned %zd", result);

	close(file);
	munmap(mapped, blob_size);
	close(fd);
	std::printf("%d failure(s)\n", failures);
	return failures;
}
