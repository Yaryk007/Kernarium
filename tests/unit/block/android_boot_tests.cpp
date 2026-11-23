// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_boot.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace android = uml::android;
namespace fs = std::filesystem;
using uml::testing::require;
using uml::testing::requireThrows;

constexpr std::uint64_t sectorSize = android::sectorSize;
constexpr std::uint64_t mebibyte = android::mebibyte;
constexpr std::uint64_t gibibyte = android::gibibyte;
constexpr std::uint64_t pageSize = 4096;
/* The ramdisks the fixture's boot images carry (android_release_fixture.py). */
constexpr std::uint64_t normalRamdiskSize = 8192;
constexpr std::uint64_t recoveryRamdiskSize = 12288;

std::string readText(const fs::path &path)
{
	std::FILE *file = std::fopen(path.c_str(), "rb");
	require(file != nullptr);
	std::string text(static_cast<std::size_t>(fs::file_size(path)), '\0');
	require(std::fread(text.data(), 1, text.size(), file) == text.size());
	require(std::fclose(file) == 0);
	return text;
}

void writeText(const fs::path &path, std::string_view text)
{
	std::FILE *file = std::fopen(path.c_str(), "wb");
	require(file != nullptr);
	require(std::fwrite(text.data(), 1, text.size(), file) == text.size());
	require(std::fclose(file) == 0);
}

std::string replaceOnce(std::string text, std::string_view from,
			std::string_view to)
{
	const auto first = text.find(from);
	require(first != std::string::npos, "the mutation target is absent");
	require(text.find(from, first + 1U) == std::string::npos,
		"the mutation target is not unique");
	text.replace(first, from.size(), to);
	return text;
}

std::vector<std::byte> readRange(const fs::path &path, std::uint64_t offset,
				 std::size_t size)
{
	const int descriptor = open(path.c_str(), O_RDONLY);
	require(descriptor >= 0);
	std::vector<std::byte> bytes(size);
	std::size_t done = 0;
	while (done < size) {
		const auto count = pread(descriptor, bytes.data() + done, size - done,
					 static_cast<off_t>(offset + done));
		require(count > 0);
		done += static_cast<std::size_t>(count);
	}
	require(close(descriptor) == 0);
	return bytes;
}

void writeAt(const fs::path &path, std::uint64_t offset,
	     std::span<const std::byte> bytes)
{
	const int descriptor = open(path.c_str(), O_RDWR);
	require(descriptor >= 0);
	android::writeAll(descriptor, offset, bytes);
	require(close(descriptor) == 0);
}

void flipByte(const fs::path &path, std::uint64_t offset)
{
	auto byte = readRange(path, offset, 1);
	byte[0] ^= std::byte{0x80};
	writeAt(path, offset, byte);
}

std::vector<std::byte> bytesOf(std::string_view text)
{
	const auto span = std::as_bytes(std::span{text.data(), text.size()});
	return {span.begin(), span.end()};
}

std::size_t entryCount(const fs::path &directory)
{
	return static_cast<std::size_t>(std::distance(
		fs::directory_iterator(directory), fs::directory_iterator{}));
}

struct Fixture {
	fs::path root;
	fs::path release;
	fs::path referenceData;
	android::Layout layout;
};

Fixture openFixture(const fs::path &root)
{
	Fixture fixture;
	fixture.root = root;
	fixture.release = root / "release";
	fixture.referenceData = root / "reference-data-4.img";
	fixture.layout =
		android::readLayout(fixture.release / android::releaseLayout);
	return fixture;
}

/* A private copy of the release that a test may damage. */
fs::path copyRelease(const Fixture &fixture, const fs::path &directory,
		     const std::string &name)
{
	const auto copy = directory / name;
	fs::copy(fixture.release, copy, fs::copy_options::recursive);
	return copy;
}

std::uint64_t miscOffset(const android::Layout &layout)
{
	return layout.data.partition("misc").offset;
}

/* Leaves `command` and its terminator in the command field, after zeroing the
 * 2 KiB message so an earlier command leaves nothing behind. */
void requestCommand(const android::Layout &layout, const fs::path &disk,
		    const std::string &command)
{
	const std::array<std::byte, 2048> cleared {};
	writeAt(disk, miscOffset(layout), cleared);
	writeAt(disk, miscOffset(layout),
		std::as_bytes(std::span{command.data(), command.size() + 1U}));
}

std::string miscCommand(const android::Layout &layout, const fs::path &disk)
{
	const auto bytes = readRange(disk, miscOffset(layout), 32);
	std::string command;
	for (const auto value : bytes) {
		if (value == std::byte{})
			break;
		command.push_back(static_cast<char>(value));
	}
	return command;
}

/* The bytes the tests compare: the partition table's start, and the misc
 * partition with a mebibyte of its neighbours on both sides. */
std::vector<std::byte> aroundMisc(const android::Layout &layout,
				  const fs::path &disk)
{
	auto bytes = readRange(disk, 0, 16U * sectorSize);
	const auto misc = readRange(disk, miscOffset(layout) - mebibyte, 3U * mebibyte);
	bytes.insert(bytes.end(), misc.begin(), misc.end());
	return bytes;
}

fs::path newDataDisk(const android::Layout &layout, const fs::path &directory,
		     std::uint32_t gib = 4)
{
	const auto path = directory / "data.img";
	android::createDataDisk(layout, path, gib);
	return path;
}

/* ------------------------------------------------------------ layout */

void testGuid()
{
	const auto guid = android::parseGuid("0fc63daf-8483-4772-8e79-3d69d8477de4");
	const std::array<std::uint8_t, 16> expected {
		0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
		0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
	require(guid.bytes == expected);
	for (const std::string_view bad :
	     {"", "0fc63daf-8483-4772-8e79-3d69d8477de",
	      "0FC63DAF-8483-4772-8E79-3D69D8477DE4",
	      "0fc63daf84834772-8e79-3d69d8477de4-",
	      "0fc63daf-8483-4772-8e79-3d69d8477deg",
	      "{0fc63daf-8483-4772-8e79-3d69d8477de4}"})
		requireThrows([&] { (void)android::parseGuid(bad); });
}

void testLayoutParsing(const Fixture &fixture)
{
	const auto &layout = fixture.layout;
	const std::array<std::string_view, 7> order {
		"init_boot", "recovery", "system", "system_ext", "vendor", "product", "odm"};
	require(layout.systemPartitions.size() == order.size());
	/* The 17 KiB of tables round up to LBA 40; every partition follows its
	 * predecessor and wastes under a page. */
	std::uint64_t cursor = 40U * sectorSize;
	for (std::size_t index = 0; index < order.size(); ++index) {
		const auto &partition = layout.systemPartitions[index];
		require(partition.name == order[index]);
		require(partition.offset == cursor);
		require(partition.offset % pageSize == 0 && partition.size % pageSize == 0);
		require(partition.size >= partition.imageBytes &&
			partition.size - partition.imageBytes < pageSize);
		cursor += partition.size;
	}
	require(layout.systemPartition("init_boot").imageBytes == pageSize + normalRamdiskSize);
	require(layout.systemPartition("odm").imageBytes == 4097);
	require(layout.systemDiskBytes ==
		fs::file_size(fixture.release / android::releaseSystemDisk));
	require(layout.systemDiskBytes % pageSize == 0);
	require(layout.systemDiskSha256.size() == 64);

	require(layout.data.layoutVersion == 1);
	require(layout.data.minimumGiB == 4 && layout.data.defaultGiB == 8 &&
		layout.data.maximumGiB == 128);
	require(layout.data.partition("misc").offset == mebibyte);
	require(layout.data.partition("misc").size == mebibyte);
	require(layout.data.partition("metadata").offset == 2U * mebibyte);
	require(layout.data.partition("metadata").size == 16U * mebibyte);
	require(layout.data.partition("userdata").offset == 64U * mebibyte);
	require(!layout.data.partition("userdata").size.has_value());
	require(layout.data.trailingFreeBytes == mebibyte);
	requireThrows([&] { (void)layout.systemPartition("userdata"); });
	requireThrows([&] { (void)layout.data.partition("system"); });

	/* The data disk's capacity is the template's range. */
	for (const std::uint32_t gib : {0U, 3U, 129U})
		requireThrows([&] { (void)layout.dataGpt(gib); });
	require(layout.dataGpt(4).diskBytes == 4U * gibibyte);
}

void testLayoutRefusals(const Fixture &fixture)
{
	const auto text = readText(fixture.release / android::releaseLayout);
	(void)android::parseLayout(text);
	const auto &layout = fixture.layout;
	const auto start = [&](std::size_t index, std::uint64_t offset) {
		return std::pair{
			"\"start_bytes\": " + std::to_string(layout.systemPartitions[index].offset),
			"\"start_bytes\": " + std::to_string(offset)};
	};
	const auto misaligned = start(0, layout.systemPartitions[0].offset + 512U);
	const auto gap = start(1, layout.systemPartitions[1].offset + pageSize);
	const auto digestWithCapital = [&] {
		auto digest = layout.systemPartitions[2].sha256;
		const auto letter = digest.find_first_of("abcdef");
		require(letter != std::string::npos);
		digest[letter] = static_cast<char>(digest[letter] - 32);
		return digest;
	}();

	struct Mutation {
		const char *label;
		std::string document;
	};
	const std::vector<Mutation> mutations {
		{"version", replaceOnce(text, "\"schema_version\": 2", "\"schema_version\": 3")},
		{"sector size", replaceOnce(text, "\"sector_size\": 512", "\"sector_size\": 4096")},
		{"duplicate key", replaceOnce(text, "\"sector_size\": 512,",
					      "\"sector_size\": 512,\n  \"sector_size\": 512,")},
		{"unknown key", replaceOnce(text, "\"sector_size\": 512,",
					    "\"sector_size\": 512,\n  \"extra\": 1,")},
		{"fraction", replaceOnce(text, "\"sector_size\": 512", "\"sector_size\": 512.0")},
		{"negative", replaceOnce(text, "\"sector_size\": 512", "\"sector_size\": -512")},
		{"leading zero", replaceOnce(text, "\"sector_size\": 512", "\"sector_size\": 0512")},
		{"huge integer", replaceOnce(text, "\"sector_size\": 512",
					     "\"sector_size\": 99999999999999999999999")},
		{"name case", replaceOnce(text, "\"name\": \"odm\"", "\"name\": \"Odm\"")},
		{"name repeats a data name", replaceOnce(text, "\"name\": \"odm\"", "\"name\": \"misc\"")},
		{"name repeats", replaceOnce(text, "\"name\": \"odm\"", "\"name\": \"vendor\"")},
		{"no recovery", replaceOnce(text, "\"name\": \"recovery\"", "\"name\": \"recoverx\"")},
		{"misaligned start", replaceOnce(text, misaligned.first, misaligned.second)},
		{"gap before a partition", replaceOnce(text, gap.first, gap.second)},
		{"not exact fit",
		 replaceOnce(text, "\"size_bytes\": " + std::to_string(layout.systemPartitions[0].size),
			     "\"size_bytes\": " + std::to_string(layout.systemPartitions[0].size + pageSize))},
		{"upper-case digest",
		 replaceOnce(text, layout.systemPartitions[2].sha256, digestWithCapital)},
		{"truncated", text.substr(0, text.size() / 2)},
		{"trailing text", text + "x"},
		{"unicode escape", replaceOnce(text, "\"sector_size\"", "\"sector\\u005fsize\"")},
		{"non-ASCII", replaceOnce(text, "\"sector_size\"", "\"sector_size\xc3\xa9\"")},
		{"capacity minimum zero", replaceOnce(text, "\"minimum\": 4", "\"minimum\": 0")},
		{"capacity default below minimum", replaceOnce(text, "\"default\": 8", "\"default\": 2")},
		{"userdata with a size",
		 replaceOnce(text, "\"size_bytes\": null", "\"size_bytes\": 1048576")},
		{"metadata overlaps misc", replaceOnce(text, "\"start_bytes\": 2097152",
						      "\"start_bytes\": 1048576")},
		{"data partition out of order",
		 replaceOnce(text, "\"name\": \"metadata\"", "\"name\": \"userdata\"")},
	};
	for (const auto &mutation : mutations) {
		bool thrown = false;
		try {
			(void)android::parseLayout(mutation.document);
		} catch (const std::invalid_argument &) {
			thrown = true;
		}
		if (!thrown)
			uml::testing::fail(std::string{"layout accepted: "} + mutation.label);
	}
	requireThrows([] { (void)android::parseLayout(""); });
	requireThrows([] { (void)android::parseLayout("[]"); });
	requireThrows([] { (void)android::parseLayout(std::string(2U * mebibyte, ' ')); });
	/* Deep nesting stops at a bound instead of the stack. */
	requireThrows([] {
		(void)android::parseLayout(std::string(200, '[') + std::string(200, ']'));
	});
}

/* -------------------------------------------------------- system disk */

void testSystemDisk(const Fixture &fixture, const fs::path &directory)
{
	const auto data = newDataDisk(fixture.layout, directory);
	const auto release = copyRelease(fixture, directory, "tampered");
	const auto system = release / android::releaseSystemDisk;
	const android::Storage storage{release, data};
	require(storage.systemDisk() == system);

	const auto &layout = fixture.layout;
	const auto backupEntries = layout.systemGpt().backupEntriesLba() * sectorSize;
	const auto last = layout.systemDiskBytes - sectorSize;
	/* Every region of the metadata is compared, not only the fields the
	 * kernel reads. */
	for (const auto offset : {std::uint64_t{450}, sectorSize + 20U,
				  sectorSize + 40U, 2U * sectorSize + 5U,
				  2U * sectorSize + 128U + 56U, backupEntries + 3U,
				  last + 24U, last + 88U}) {
		flipByte(system, offset);
		requireThrows([&] { storage.validate(); });
		requireThrows([&] { (void)android::Storage{release, data}; });
		flipByte(system, offset);
		storage.validate();
	}

	/* Payload bytes are not read at boot; the installer verified them. */
	flipByte(system, layout.systemPartition("odm").offset + 5U);
	storage.validate();
	flipByte(system, layout.systemPartition("odm").offset + 5U);

	/* The size is exact, and the old single disk is not a system disk. */
	for (const std::uint64_t size : {layout.systemDiskBytes + pageSize,
					 layout.systemDiskBytes - pageSize,
					 std::uint64_t{3'274'702'848ULL}}) {
		const auto sized = copyRelease(fixture, directory, "sized");
		fs::resize_file(sized / android::releaseSystemDisk, size);
		requireThrows([&] { (void)android::Storage{sized, data}; });
		fs::remove_all(sized);
	}

	/* A link is not a disk. */
	const auto moved = release / "guest/system.moved";
	fs::rename(system, moved);
	fs::create_symlink(moved, system);
	requireThrows([&] { storage.validate(); });
	fs::remove(system);
	fs::rename(moved, system);
	storage.validate();

	/* Neither is a missing layout, a layout of another schema, or a layout
	 * that is not a file. */
	const auto layoutPath = release / android::releaseLayout;
	const auto layoutText = readText(layoutPath);
	writeText(layoutPath,
		  replaceOnce(layoutText, "\"schema_version\": 2", "\"schema_version\": 1"));
	requireThrows([&] { (void)android::Storage{release, data}; });
	fs::remove(layoutPath);
	requireThrows([&] { (void)android::Storage{release, data}; });
	fs::create_directory(layoutPath);
	requireThrows([&] { (void)android::Storage{release, data}; });
	fs::remove(layoutPath);
	writeText(layoutPath, layoutText);
	(void)android::Storage{release, data};
	fs::remove(data);
}

/* ---------------------------------------------------------- data disk */

void requireTablesMatch(const fs::path &left, const fs::path &right)
{
	/* Both ends of the disks: the tables, and a mebibyte past each. */
	const auto size = fs::file_size(left);
	require(size == fs::file_size(right));
	require(readRange(left, 0, 2 * mebibyte) == readRange(right, 0, 2 * mebibyte));
	require(readRange(left, size - mebibyte, mebibyte) ==
		readRange(right, size - mebibyte, mebibyte));
}

std::uint64_t allocatedBytes(const fs::path &path)
{
	struct stat details {};
	require(stat(path.c_str(), &details) == 0);
	return static_cast<std::uint64_t>(details.st_blocks) * 512U;
}

/* What a disk is as a file: the same inode and modification time prove that
 * nothing wrote it or replaced it, the blocks that nothing was allocated. */
struct FileIdentity {
	ino_t inode;
	off_t size;
	timespec modified;
	blkcnt_t blocks;

	bool operator==(const FileIdentity &other) const
	{
		return inode == other.inode && size == other.size &&
		       modified.tv_sec == other.modified.tv_sec &&
		       modified.tv_nsec == other.modified.tv_nsec &&
		       blocks == other.blocks;
	}
};

FileIdentity identityOf(const fs::path &path)
{
	struct stat details {};
	require(stat(path.c_str(), &details) == 0);
	return {details.st_ino, details.st_size, details.st_mtimespec,
		details.st_blocks};
}

/* A child that holds the disk open and locked as a running guest does, until
 * the object is destroyed. */
class LockHolder final {
public:
	explicit LockHolder(const fs::path &path)
	{
		int ready[2];
		int release[2];
		require(pipe(ready) == 0 && pipe(release) == 0);
		child_ = fork();
		require(child_ >= 0);
		if (child_ == 0) {
			close(ready[0]);
			close(release[1]);
			const int descriptor = open(path.c_str(), O_RDWR);
			struct flock lock {};
			lock.l_type = F_WRLCK;
			lock.l_whence = SEEK_SET;
			const char status = descriptor >= 0 &&
				fcntl(descriptor, F_SETLK, &lock) == 0 ? 'y' : 'n';
			if (write(ready[1], &status, 1) != 1)
				_exit(1);
			char wait = 0;
			if (read(release[0], &wait, 1) < 0)
				_exit(1);
			_exit(0);
		}
		close(ready[1]);
		close(release[0]);
		release_ = release[1];
		char status = 'n';
		require(read(ready[0], &status, 1) == 1 && status == 'y');
		close(ready[0]);
	}
	~LockHolder()
	{
		close(release_);
		int status = 0;
		(void)waitpid(child_, &status, 0);
	}
	LockHolder(const LockHolder &) = delete;
	LockHolder &operator=(const LockHolder &) = delete;

private:
	pid_t child_ = -1;
	int release_ = -1;
};

void testDataDiskCreate(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto path = directory / "data.img";
	android::createDataDisk(layout, path, 4);

	/* The host's tables are the producer tool's, byte for byte. */
	requireTablesMatch(path, fixture.referenceData);
	require(android::validateDataDisk(layout, path) == 4);
	require(fs::file_size(path) == 4U * gibibyte);
	/* Only the tables are written: a disk of any capacity costs no more. */
	require(allocatedBytes(path) < 8U * mebibyte);
	struct stat details {};
	require(stat(path.c_str(), &details) == 0);
	require((details.st_mode & 0077U) == 0);
	/* misc is zero, which reads as no request. */
	require(miscCommand(layout, path).empty());

	/* Nothing is left beside it, and it is never replaced. */
	require(entryCount(directory) == 1);
	const auto before = readRange(path, 0, 2 * mebibyte);
	requireThrows<std::system_error>([&] { android::createDataDisk(layout, path, 6); });
	require(readRange(path, 0, 2 * mebibyte) == before);
	require(fs::file_size(path) == 4U * gibibyte);
	require(entryCount(directory) == 1);

	for (const std::uint32_t gib : {0U, 3U, 129U})
		requireThrows([&] { android::createDataDisk(layout, directory / "other.img", gib); });
	for (const char *name : {"a,b.img", "a b.img", "a:b.img"})
		requireThrows([&] { android::createDataDisk(layout, directory / name, 4); });
	require(entryCount(directory) == 1);

	/* Through a link, not even into place. */
	const auto link = directory / "link.img";
	fs::create_symlink(path, link);
	requireThrows([&] { (void)android::validateDataDisk(layout, link); });
	requireThrows<std::system_error>([&] { android::createDataDisk(layout, link, 4); });
	fs::remove(link);
	fs::remove(path);
}

void testDataDiskValidation(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto path = newDataDisk(layout, directory);
	require(android::validateDataDisk(layout, path) == 4);

	/* Every byte of the tables matters. */
	const auto size = fs::file_size(path);
	for (const auto offset : {std::uint64_t{450}, sectorSize + 20U, sectorSize + 56U,
				  2U * sectorSize + 40U, 2U * sectorSize + 128U + 32U,
				  size - sectorSize + 24U, size - 33U * sectorSize + 16U}) {
		flipByte(path, offset);
		requireThrows([&] { (void)android::validateDataDisk(layout, path); });
		flipByte(path, offset);
		require(android::validateDataDisk(layout, path) == 4);
	}

	/* The capacity is whole GiB within the template's range. */
	fs::resize_file(path, size + pageSize);
	requireThrows([&] { (void)android::validateDataDisk(layout, path); });
	fs::resize_file(path, size + gibibyte);
	requireThrows([&] { (void)android::validateDataDisk(layout, path); });
	fs::resize_file(path, size);
	require(android::validateDataDisk(layout, path) == 4);

	/* A file that is not read-write, not regular, or a data disk of another
	 * layout. */
	require(chmod(path.c_str(), S_IRUSR) == 0);
	requireThrows([&] { (void)android::validateDataDisk(layout, path); });
	require(chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);
	requireThrows([&] { (void)android::validateDataDisk(layout, directory); });
	requireThrows([&] { (void)android::validateDataDisk(layout, directory / "missing.img"); });
	requireThrows([&] {
		(void)android::validateDataDisk(layout, fixture.release / android::releaseSystemDisk);
	});

	const auto changed = replaceOnce(
		readText(fixture.release / android::releaseLayout),
		"\"start_bytes\": 67108864", "\"start_bytes\": 134217728");
	const auto changedLayout = android::parseLayout(changed);
	requireThrows([&] { (void)android::validateDataDisk(changedLayout, path); });
	fs::remove(path);
}

void testDataDiskGrow(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;

	/* An absent disk is created at the template's default, or at the capacity
	 * asked for; a present one is left alone unless it must grow. */
	const auto path = directory / "data.img";
	auto prepared = android::prepareDataDisk(layout, path, 0);
	require(prepared.change == android::DataDiskChange::created &&
		prepared.capacityGiB == 8);
	require(fs::file_size(path) == 8U * gibibyte);
	prepared = android::prepareDataDisk(layout, path, 0);
	require(prepared.change == android::DataDiskChange::none && prepared.capacityGiB == 8);
	prepared = android::prepareDataDisk(layout, path, 8);
	require(prepared.change == android::DataDiskChange::none && prepared.capacityGiB == 8);
	fs::remove(path);
	prepared = android::prepareDataDisk(layout, path, 5);
	require(prepared.change == android::DataDiskChange::created &&
		prepared.capacityGiB == 5);

	/* Data the guest wrote survives the grow, and stays where it was. */
	const auto userdata = layout.data.partition("userdata").offset;
	const auto metadata = layout.data.partition("metadata").offset;
	const auto marker = bytesOf("a block of userdata");
	writeAt(path, userdata + 12345U, marker);
	writeAt(path, metadata + 77U, marker);
	requestCommand(layout, path, "boot-recovery");

	/* The old backup tables lie in what becomes userdata. */
	const auto oldEnd = 5U * gibibyte;
	require(readRange(path, oldEnd - sectorSize, 1)[0] == std::byte{'E'});
	prepared = android::prepareDataDisk(layout, path, 7);
	require(prepared.change == android::DataDiskChange::grown && prepared.capacityGiB == 7);
	require(fs::file_size(path) == 7U * gibibyte);
	require(android::validateDataDisk(layout, path) == 7);
	require(readRange(path, userdata + 12345U, marker.size()) == marker);
	require(readRange(path, metadata + 77U, marker.size()) == marker);
	require(miscCommand(layout, path) == "boot-recovery");
	const auto stale = readRange(path, oldEnd - 34U * sectorSize, 34U * sectorSize);
	require(std::all_of(stale.begin(), stale.end(),
			    [](std::byte value) { return value == std::byte{}; }));
	require(allocatedBytes(path) < 8U * mebibyte);
	/* A grown disk is a fresh disk of that size, apart from what the guest
	 * wrote. */
	const auto fresh = directory / "fresh.img";
	android::createDataDisk(layout, fresh, 7);
	require(readRange(path, 7U * gibibyte - mebibyte, mebibyte) ==
		readRange(fresh, 7U * gibibyte - mebibyte, mebibyte));
	fs::remove(fresh);

	/* A request below the disk keeps the disk: the caller asked for a size
	 * the disk already exceeds, as an application does after the disk grew
	 * for a start that never recorded it. Nothing is written, replaced or
	 * left beside it. */
	{
		const auto before = identityOf(path);
		const auto head = readRange(path, 0, 2 * mebibyte);
		const auto tail = readRange(path, 7U * gibibyte - mebibyte, mebibyte);
		for (const std::uint32_t smaller : {6U, 1U}) {
			prepared = android::prepareDataDisk(layout, path, smaller);
			require(prepared.change == android::DataDiskChange::none &&
				prepared.capacityGiB == 7);
		}
		require(identityOf(path) == before);
		require(android::validateDataDisk(layout, path) == 7);
		require(readRange(path, 0, 2 * mebibyte) == head);
		require(readRange(path, 7U * gibibyte - mebibyte, mebibyte) == tail);
		require(readRange(path, userdata + 12345U, marker.size()) == marker);
		require(readRange(path, metadata + 77U, marker.size()) == marker);
		require(miscCommand(layout, path) == "boot-recovery");
		require(entryCount(directory) == 1);
	}

	/* growDataDisk keeps its own precondition: it grows or it throws. */
	requireThrows([&] { android::growDataDisk(layout, path, 7); });
	requireThrows([&] { android::growDataDisk(layout, path, 6); });
	requireThrows([&] { android::growDataDisk(layout, path, 129); });
	/* A request beyond the template's range still fails, and the disk stays. */
	requireThrows([&] { (void)android::prepareDataDisk(layout, path, 129); });
	require(android::validateDataDisk(layout, path) == 7);
	require(entryCount(directory) == 1);

	/* A disk a guest holds open is not grown under it. */
	{
		require(!android::lockedByAnotherProcess(path));
		const LockHolder holder{path};
		require(android::lockedByAnotherProcess(path));
		requireThrows<std::runtime_error>([&] { android::growDataDisk(layout, path, 9); });
		require(android::validateDataDisk(layout, path) == 7);
	}
	/* The kernel's lock is an open file description lock, so a guest in this
	 * process holds the disk as well, and the probe's own descriptor, opened
	 * and closed by each look, does not end it. */
	{
		const int held = open(path.c_str(), O_RDWR);
		require(held >= 0);
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		require(fcntl(held, F_OFD_SETLK, &lock) == 0);
		require(android::lockedByAnotherProcess(path));
		require(android::lockedByAnotherProcess(path),
			"the probe ended the lock it looked for");
		requireThrows<std::runtime_error>([&] { android::growDataDisk(layout, path, 9); });
		require(close(held) == 0);
	}
	require(!android::lockedByAnotherProcess(path));
	android::growDataDisk(layout, path, 64);
	require(android::validateDataDisk(layout, path) == 64);
	require(readRange(path, userdata + 12345U, marker.size()) == marker);
	require(entryCount(directory) == 1);

	/* A grow that cannot start leaves the disk as it was. */
	const auto damaged = directory / "damaged.img";
	android::createDataDisk(layout, damaged, 4);
	flipByte(damaged, sectorSize + 20U);
	requireThrows([&] { android::growDataDisk(layout, damaged, 6); });
	require(fs::file_size(damaged) == 4U * gibibyte);
	fs::remove(damaged);
	fs::remove(path);
}

/* ------------------------------------------------------------- boot */

void testStorageBoot(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto data = newDataDisk(layout, directory);
	const android::Storage storage{fixture.release, data};
	const auto &initBoot = layout.systemPartition("init_boot");
	const auto &recovery = layout.systemPartition("recovery");

	/* The data disk's bootloader message picks the ramdisk. */
	const auto automatic = storage.selectBoot(android::BootMode::automatic);
	require(automatic.mode == android::BootMode::normal);
	require(automatic.ramdisk.offset == initBoot.offset + pageSize);
	require(automatic.ramdisk.size == normalRamdiskSize);
	const auto forced = storage.selectBoot(android::BootMode::recovery);
	require(forced.mode == android::BootMode::recovery);
	require(forced.ramdisk.offset == recovery.offset + pageSize);
	require(forced.ramdisk.size == recoveryRamdiskSize);
	require(storage.selectBoot(android::BootMode::normal).mode ==
		android::BootMode::normal);
	requestCommand(layout, data, "boot-recovery");
	require(storage.selectBoot(android::BootMode::automatic).mode ==
		android::BootMode::recovery);
	require(storage.selectBoot(android::BootMode::normal).mode ==
		android::BootMode::normal);
	requestCommand(layout, data, "");
	requireThrows([&] { (void)storage.selectBoot(static_cast<android::BootMode>(9)); });

	/* The kernel arguments attach the system disk read-only as the first
	 * disk, the data disk as the second, and nothing else. */
	const auto system = (fixture.release / android::releaseSystemDisk).string();
	const auto arguments = storage.kernelArguments(automatic);
	require((arguments == std::vector<std::string>{
		"ubd0r=" + system,
		"ubd1=" + data.string(),
		"initrd=" + system,
		"initrd_offset=" + std::to_string(initBoot.offset + pageSize),
		"initrd_size=" + std::to_string(normalRamdiskSize),
		"root=/dev/ram0",
		"rootfstype=ramfs"}));

	/* A damaged data disk stops the next boot. */
	flipByte(data, sectorSize + 20U);
	requireThrows([&] { (void)storage.selectBoot(android::BootMode::normal); });
	requireThrows([&] { (void)storage.consumeBootloaderCommand(); });
	requireThrows([&] { (void)storage.selectRestartBoot(); });
	flipByte(data, sectorSize + 20U);
	(void)storage.selectBoot(android::BootMode::normal);

	/* Boot partitions that are not kernelless v4 images, or whose ramdisk
	 * does not fit the image, are refused for either boot. */
	const auto release = copyRelease(fixture, directory, "boot-images");
	const android::Storage damaged{release, data};
	const auto disk = release / android::releaseSystemDisk;
	flipByte(disk, initBoot.offset);
	for (const auto mode : {android::BootMode::normal, android::BootMode::recovery})
		requireThrows([&] { (void)damaged.selectBoot(mode); });
	flipByte(disk, initBoot.offset);
	(void)damaged.selectBoot(android::BootMode::normal);
	writeAt(disk, recovery.offset + 12U + 3U, bytesOf(std::string(1, '\x7f')));
	for (const auto mode : {android::BootMode::normal, android::BootMode::recovery})
		requireThrows([&] { (void)damaged.selectBoot(mode); });
	fs::remove_all(release);
	fs::remove(data);
}

/* There is no bootloader, so a restart consumes the one-shot request `reboot
 * bootloader` leaves in the misc partition, as a bootloader that continues
 * does: only the command field changes, and nothing else is consumed. */
void testBootloaderCommandIsConsumed(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto disk = newDataDisk(layout, directory);
	const android::Storage storage{fixture.release, disk};
	const std::string bootloader = "bootonce-bootloader";

	/* Nothing to consume leaves the disk alone. */
	const auto pristine = aroundMisc(layout, disk);
	require(!storage.consumeBootloaderCommand());
	require(aroundMisc(layout, disk) == pristine);

	/* The request is cleared, and only the 32-byte command field changes. A
	 * status, recovery and stage that follow it in the message stay. */
	requestCommand(layout, disk, bootloader);
	writeAt(disk, miscOffset(layout) + 32U + 32U, bytesOf("recovery-field-stays"));
	const auto requested = aroundMisc(layout, disk);
	require(miscCommand(layout, disk) == bootloader);
	require(storage.consumeBootloaderCommand());
	auto expected = requested;
	std::fill_n(expected.begin() + static_cast<std::ptrdiff_t>(16U * sectorSize) +
			    static_cast<std::ptrdiff_t>(mebibyte),
		    32, std::byte{});
	require(aroundMisc(layout, disk) == expected);
	require(miscCommand(layout, disk).empty());
	require(!storage.consumeBootloaderCommand());

	/* Recovery's request is recovery's to clear, not the host's. */
	requestCommand(layout, disk, "boot-recovery");
	const auto recovery = aroundMisc(layout, disk);
	require(!storage.consumeBootloaderCommand());
	require(aroundMisc(layout, disk) == recovery);

	/* Only the exact request: a longer or shorter name, and a field with no
	 * terminator, are not it. */
	for (const std::string other : {"bootonce-bootloader2", "bootonce-bootload",
					"boot-bootloader", "boot-fastboot"}) {
		requestCommand(layout, disk, other);
		require(!storage.consumeBootloaderCommand());
		require(miscCommand(layout, disk) == other);
	}
	writeAt(disk, miscOffset(layout), bytesOf(std::string(32, 'b')));
	require(!storage.consumeBootloaderCommand());
	requireThrows([&] { (void)storage.selectBoot(android::BootMode::automatic); });
	(void)storage.selectBoot(android::BootMode::normal);

	/* A request the host cannot clear is an error, not a silent boot. */
	requestCommand(layout, disk, bootloader);
	require(chmod(disk.c_str(), S_IRUSR) == 0);
	requireThrows([&] { (void)storage.consumeBootloaderCommand(); });
	require(chmod(disk.c_str(), S_IRUSR | S_IWUSR) == 0);
	require(miscCommand(layout, disk) == bootloader);
	fs::remove(disk);
}

/* Runs `body` on a thread of its own and fails, instead of hanging, when it
 * has not returned within `limit`. */
template <typename Body>
void requireReturns(std::chrono::seconds limit, const char *what, Body body)
{
	auto finished = std::make_shared<std::promise<void>>();
	auto done = finished->get_future();
	std::thread([finished, &body] {
		body();
		finished->set_value();
	}).detach();
	require(done.wait_for(limit) == std::future_status::ready, what);
}

/* The release's own root image (the Magisk-patched copy of init_boot, beside
 * the system disk) replaces the normal boot's ramdisk on every boot and
 * restart when the guest is asked for root. Only its ramdisk is used, recovery
 * is untouched, and anything malformed, absent or linked is refused. */
void testRootBootImage(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto data = newDataDisk(layout, directory);
	const auto &initBoot = layout.systemPartition("init_boot");
	const auto release = copyRelease(fixture, directory, "root-release");
	const android::Storage storage{release, data};
	const auto system = (release / android::releaseSystemDisk).string();
	const auto image = release / android::releaseRootBootImage;

	/* A release with no root image boots as before and refuses the option;
	 * recovery is not asked for it. */
	require(!storage.carriesRoot());
	require(storage.selectBoot(android::BootMode::normal).ramdiskFile.empty());
	requireThrows([&] {
		(void)storage.selectBoot(android::BootMode::normal, android::RootMode::on);
	});
	requireThrows([&] { (void)storage.selectRestartBoot(android::RootMode::on); });
	requireThrows([&] {
		(void)storage.selectBoot(android::BootMode::normal,
					 static_cast<android::RootMode>(7));
	});

	/* The release's init_boot as the root image, its ramdisk made a newc
	 * archive by its first bytes. */
	fs::create_directory(release / "root");
	{
		const auto whole = readRange(release / android::releaseSystemDisk,
					     initBoot.offset, initBoot.imageBytes);
		std::FILE *file = std::fopen(image.c_str(), "wb");
		require(file != nullptr);
		require(std::fwrite(whole.data(), 1, whole.size(), file) == whole.size());
		require(std::fclose(file) == 0);
	}
	writeAt(image, pageSize, bytesOf("070701"));
	require(storage.carriesRoot());

	const auto taken = storage.selectBoot(android::BootMode::normal, android::RootMode::on);
	require(taken.mode == android::BootMode::normal);
	require(taken.kind() == android::RamdiskKind::root);
	require(taken.ramdiskFile == image);
	require(taken.ramdisk.offset == pageSize);
	require(taken.ramdisk.size == normalRamdiskSize);
	require((storage.kernelArguments(taken) == std::vector<std::string>{
		"ubd0r=" + system,
		"ubd1=" + data.string(),
		"initrd=" + image.string(),
		"initrd_offset=" + std::to_string(pageSize),
		"initrd_size=" + std::to_string(normalRamdiskSize),
		"root=/dev/ram0",
		"rootfstype=ramfs"}));
	/* Automatic mode and a restart take it too; without the option the
	 * system disk's own ramdisk is still the one. */
	require(storage.selectBoot(android::BootMode::automatic, android::RootMode::on)
			.ramdiskFile == image);
	require(storage.selectRestartBoot(android::RootMode::on).boot.ramdiskFile == image);
	const auto stock = storage.selectBoot(android::BootMode::normal);
	require(stock.kind() == android::RamdiskKind::stock);
	require(stock.ramdiskFile.empty());
	require(stock.ramdisk.offset == initBoot.offset + pageSize);
	require(storage.kernelArguments(stock)[2] == "initrd=" + system);
	require(storage.selectRestartBoot().boot.ramdiskFile.empty());

	/* Recovery never takes it, and does not read it. */
	const auto recovery = storage.selectBoot(android::BootMode::recovery, android::RootMode::on);
	require(recovery.kind() == android::RamdiskKind::recovery);
	require(recovery.ramdiskFile.empty());
	require(recovery.ramdisk.offset == layout.systemPartition("recovery").offset + pageSize);
	requestCommand(layout, data, "boot-recovery");
	require(storage.selectRestartBoot(android::RootMode::on).boot.ramdiskFile.empty());
	/* An automatic restart into recovery boots recovery's ramdisk, not the
	 * root image the run asked for: the boot is recovery's. */
	require(storage.selectRestartBoot(android::RootMode::on).boot.kind() ==
		android::RamdiskKind::recovery);
	requestCommand(layout, data, "");

	const auto refuse = [&] {
		requireThrows([&] {
			(void)storage.selectBoot(android::BootMode::normal, android::RootMode::on);
		});
		/* The stock boot never reads the root image. */
		require(storage.selectBoot(android::BootMode::normal).ramdiskFile.empty());
	};
	/* An image that is not a regular file the release carries. */
	const auto original = release / "root" / "original.img";
	fs::rename(image, original);
	refuse();
	fs::create_symlink(original, image);
	require(!storage.carriesRoot());
	refuse();
	fs::remove(image);
	fs::create_directory(image);
	require(!storage.carriesRoot());
	refuse();
	fs::remove(image);
	/* A pipe with no writer is refused too, and opening it does not wait for
	 * one: the check of the type runs on the descriptor, after the open. */
	require(mkfifo(image.c_str(), 0600) == 0);
	requireReturns(std::chrono::seconds(10),
		"opening a pipe at the root image's name did not return", [&] {
			require(!storage.carriesRoot());
			refuse();
		});
	fs::remove(image);
	fs::rename(original, image);
	/* `root/` itself is a real directory, never a link to one, whatever the
	 * image inside is; and a file in its place is no directory. */
	const auto elsewhere = release / "elsewhere";
	fs::rename(release / "root", elsewhere);
	fs::create_directory_symlink(elsewhere, release / "root");
	require(!storage.carriesRoot());
	refuse();
	fs::remove(release / "root");
	{
		std::FILE *file = std::fopen((release / "root").c_str(), "wb");
		require(file != nullptr);
		require(std::fclose(file) == 0);
	}
	require(!storage.carriesRoot());
	refuse();
	fs::remove(release / "root");
	fs::rename(elsewhere, release / "root");
	require(storage.carriesRoot());

	/* The header: magic, kernel size, header size, version, reserved words
	 * and the signature size, as the release's own partitions are checked. */
	const auto mutate = [&](std::uint64_t offset, const char *replacement,
				std::size_t length) {
		fs::copy_file(image, original, fs::copy_options::overwrite_existing);
		writeAt(image, offset, bytesOf(std::string(replacement, length)));
		refuse();
		fs::rename(original, image);
	};
	mutate(0, "ANDROID?", 8);
	mutate(8, "\x01", 1);	 /* a kernel the host never starts */
	mutate(20, "\x00", 1);	 /* header size */
	mutate(40, "\x03", 1);	 /* header version 3 */
	mutate(24, "\x01", 1);	 /* reserved */
	mutate(1580, "\x01", 1); /* boot signature size */
	/* A ramdisk that is empty or does not fit the file. */
	mutate(12, "\x00\x00\x00\x00", 4);
	mutate(12, "\xff\xff\xff\x7f", 4);
	mutate(12, "\x01\x20\x00\x00", 4); /* one byte past the file */
	/* A ramdisk in no format the kernel unpacks. */
	mutate(pageSize, "junk", 4);
	/* The magic is the ramdisk's own, not the file's padding after it: a
	 * ramdisk of one byte followed by padding that completes gzip's. */
	fs::copy_file(image, original, fs::copy_options::overwrite_existing);
	writeAt(image, 12, bytesOf(std::string("\x01\x00\x00\x00", 4)));
	writeAt(image, pageSize, bytesOf(std::string("\x1f\x8b", 2)));
	refuse();
	fs::rename(original, image);
	/* Every format the kernel unpacks is taken. */
	using namespace std::string_view_literals;
	for (const auto magic : {"\x1f\x8b"sv, "BZh"sv, "\xfd" "7zXZ\x00"sv,
				 "\x5d\x00\x00"sv, "\x89LZO"sv, "\x02\x21\x4c\030"sv,
				 "\x28\xb5\x2f\xfd"sv, "070702"sv}) {
		fs::copy_file(image, original, fs::copy_options::overwrite_existing);
		writeAt(image, pageSize, bytesOf(magic));
		(void)storage.selectBoot(android::BootMode::normal, android::RootMode::on);
		fs::rename(original, image);
	}
	/* A file with no ramdisk page. */
	fs::copy_file(image, original, fs::copy_options::overwrite_existing);
	fs::resize_file(image, pageSize);
	refuse();
	fs::rename(original, image);
	/* The size bound, on the valid image grown past it: its header, ramdisk
	 * and format stay as they were, so only the bound can refuse it. 256 MiB
	 * is the most taken. */
	fs::copy_file(image, original, fs::copy_options::overwrite_existing);
	fs::resize_file(image, 256U * mebibyte);
	(void)storage.selectBoot(android::BootMode::normal, android::RootMode::on);
	fs::resize_file(image, 256U * mebibyte + 1U);
	refuse();
	fs::rename(original, image);
	(void)storage.selectBoot(android::BootMode::normal, android::RootMode::on);

	/* A damaged data disk stops a rooted boot as it stops any other. */
	flipByte(data, sectorSize + 20U);
	requireThrows([&] {
		(void)storage.selectBoot(android::BootMode::normal, android::RootMode::on);
	});
	flipByte(data, sectorSize + 20U);
	fs::remove_all(release);
	fs::remove(data);
}

/* Every restart selects the ramdisk in automatic mode, whatever the first
 * boot was given, so `reboot recovery` boots recovery and `reboot bootloader`
 * boots normally after its request is consumed. */
void testRestartSelectsAutomaticMode(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto disk = newDataDisk(layout, directory);
	const android::Storage storage{fixture.release, disk};
	const auto initBoot = layout.systemPartition("init_boot").offset + pageSize;
	const auto recovery = layout.systemPartition("recovery").offset + pageSize;

	/* A first boot forced into recovery does not make the restart one. */
	require(storage.selectBoot(android::BootMode::recovery).mode ==
		android::BootMode::recovery);
	auto restarted = storage.selectRestartBoot();
	require(restarted.boot.mode == android::BootMode::normal);
	require(restarted.boot.ramdisk.offset == initBoot);
	require(restarted.boot.ramdisk.size == normalRamdiskSize);
	require(!restarted.bootloaderConsumed);

	/* `reboot recovery`: init left boot-recovery, and recovery boots; the
	 * request stays for recovery to clear. */
	requestCommand(layout, disk, "boot-recovery");
	restarted = storage.selectRestartBoot();
	require(restarted.boot.mode == android::BootMode::recovery);
	require(restarted.boot.ramdisk.offset == recovery);
	require(restarted.boot.ramdisk.size == recoveryRamdiskSize);
	require(!restarted.bootloaderConsumed);
	require(miscCommand(layout, disk) == "boot-recovery");

	/* The same request read by an automatic selection of its own. */
	require(storage.selectBoot(android::BootMode::automatic).mode ==
		android::BootMode::recovery);

	/* Recovery's own reboot cleared the request: the next boot is normal. */
	requestCommand(layout, disk, "");
	require(storage.selectRestartBoot().boot.mode == android::BootMode::normal);

	/* `reboot bootloader`: consumed, a normal boot, and a later request for
	 * recovery works, as it did not while the request stayed. */
	requestCommand(layout, disk, "bootonce-bootloader");
	restarted = storage.selectRestartBoot();
	require(restarted.bootloaderConsumed);
	require(restarted.boot.mode == android::BootMode::normal);
	require(restarted.boot.ramdisk.offset == initBoot);
	require(miscCommand(layout, disk).empty());
	requestCommand(layout, disk, "boot-recovery");
	require(storage.selectRestartBoot().boot.mode == android::BootMode::recovery);

	/* A disk that is not the data layout stops the restart. */
	fs::resize_file(disk, 5U * gibibyte);
	requireThrows([&] { (void)storage.selectRestartBoot(); });
	fs::remove(disk);
}

/* The guest has exactly the two disks: no argument asks for another, and a
 * disk that is not what the layout says is not attached. */
void testStorageIsExact(const Fixture &fixture, const fs::path &directory)
{
	const auto &layout = fixture.layout;
	const auto data = newDataDisk(layout, directory);

	for (const char *argument : {"ubd2=/tmp/other.img", "ubd0=/tmp/other.img",
				     "ubda=/tmp/other.img", "ubd1r=/tmp/other.img", "ubd",
				     "initrd=/tmp/other.img", "initrd_offset=0",
				     "root=/dev/ubda", "rootfstype=erofs",
				     "androidboot.boot_devices=uml-blkdev.0,uml-blkdev.2"})
		requireThrows([&] {
			const std::vector<std::string> arguments{"mem=4G", argument};
			android::rejectStorageArguments(arguments);
		});
	android::rejectStorageArguments(std::vector<std::string>{
		"mem=4G", "ncpus=10", "console=ttyS0", "androidboot.hardware=kernarium"});
	android::rejectStorageArguments({});

	/* The release's own command line may not attach a disk either, and must
	 * name the two it expects. */
	constexpr std::array<std::string_view, 1> launcherPrefixes{
		"androidboot.kernarium.adb_auth="};
	const auto arguments = android::readKernelCommandLine(
		fixture.release / android::releaseCommandLine, launcherPrefixes);
	require(std::find(arguments.begin(), arguments.end(),
			  std::string{android::bootDevicesArgument}) != arguments.end());
	const auto path = directory / "command-line.txt";
	const std::string good = "quiet androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1\n";
	writeText(path, good);
	require(android::readKernelCommandLine(path, launcherPrefixes).size() == 2);
	for (const std::string &bad : std::vector<std::string>{
	      "quiet\n",
	      "quiet androidboot.boot_devices=uml-blkdev.0\n",
	      "quiet androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1,uml-blkdev.2\n",
	      "quiet androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1 "
	      "androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1\n",
	      good + "ubd2=/tmp/other.img\n", good + "initrd=/tmp/other\n",
	      good + "root=/dev/ubda\n", good + "rootfstype=erofs\n",
	      good + "androidboot.kernarium.adb_auth=0\n"}) {
		writeText(path, bad);
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, launcherPrefixes);
		});
	}

	/* The data disk may not be the system disk, nor any file that is not a
	 * data disk of this layout, nor a path a UBD argument cannot carry. */
	requireThrows([&] {
		(void)android::Storage{fixture.release, fixture.release / android::releaseSystemDisk};
	});
	requireThrows([&] { (void)android::Storage{fixture.release, fixture.release}; });
	requireThrows([&] {
		(void)android::Storage{fixture.release, directory / "missing.img"};
	});
	requireThrows([&] { (void)android::Storage{fixture.release, "data.img"}; });
	requireThrows([&] { (void)android::Storage{"release", data}; });
	const auto commaDisk = directory / "a,b.img";
	fs::rename(data, commaDisk);
	requireThrows([&] { (void)android::Storage{fixture.release, commaDisk}; });
	fs::rename(commaDisk, data);
	(void)android::Storage{fixture.release, data};

	/* A release root that holds no disk. */
	requireThrows([&] { (void)android::Storage{directory / "missing", data}; });
	fs::remove(data);
}

/* The standalone launcher gives every guest a density: the EDID's, or the one
 * SurfaceFlinger falls back to. A release cannot set it. */
void testLcdDensity(const fs::path &directory)
{
	require(android::lcdDensityArgument(0) == "androidboot.lcd_density=213");
	require(android::lcdDensityArgument(320) == "androidboot.lcd_density=320");
	require(android::lcdDensityArgument(android::defaultLcdDensity) ==
		"androidboot.lcd_density=213");

	require(android::parseDotsPerInch("160") == 160U);
	for (const std::string_view bad : {"", "0", "-1", "12x", " 5", "5 ",
					   "4294967296", "dpi"})
		requireThrows([&] { (void)android::parseDotsPerInch(bad); });

	constexpr std::array<std::string_view, 2> launcherPrefixes{
		"androidboot.kernarium.adb_auth=", android::lcdDensityPrefix};
	const auto path = directory / "density-command-line.txt";
	const std::string devices = " androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1\n";
	writeText(path, "quiet androidboot.lcd_density=240" + devices);
	requireThrows([&] { (void)android::readKernelCommandLine(path, launcherPrefixes); });
	writeText(path, "quiet androidboot.hardware=kernarium" + devices);
	require(android::readKernelCommandLine(path, launcherPrefixes) ==
		std::vector<std::string>{"quiet", "androidboot.hardware=kernarium",
					 std::string{android::bootDevicesArgument}});
}

/* Every launcher gives the guest a timeout multiplier from its vCPU count.
 * A release cannot set it, apart from the `=5` that the releases published
 * before the launchers set it carry, which a launcher that sets the value
 * drops. */
void testHwTimeoutMultiplier(const fs::path &directory)
{
	for (const unsigned vcpus : {0U, 1U, 2U, 4U, 5U})
		require(android::hwTimeoutMultiplier(vcpus) == 5U);
	for (const unsigned vcpus : {6U, 7U, 8U, 10U, 64U})
		require(android::hwTimeoutMultiplier(vcpus) == 3U);

	require(android::hwTimeoutMultiplierArgument(8) ==
		"androidboot.hw_timeout_multiplier=3");
	require(android::hwTimeoutMultiplierArgument(4) ==
		"androidboot.hw_timeout_multiplier=5");
	require(android::legacyHwTimeoutMultiplierArgument ==
		std::string{android::hwTimeoutMultiplierPrefix} + "5");

	const std::string devices{android::bootDevicesArgument};
	const auto path = directory / "multiplier-command-line.txt";
	constexpr std::array<std::string_view, 2> setsMultiplier{
		"androidboot.kernarium.adb_auth=",
		android::hwTimeoutMultiplierPrefix};
	constexpr std::array<std::string_view, 1> leavesMultiplier{
		"androidboot.kernarium.adb_auth="};

	/* The launcher that sets the multiplier drops the legacy argument and
	 * keeps the others in order. */
	writeText(path, "quiet androidboot.hw_timeout_multiplier=5 " + devices +
			" console=ttyS0\n");
	require(android::readKernelCommandLine(path, setsMultiplier) ==
		std::vector<std::string>{"quiet", devices, "console=ttyS0"});
	/* A release without the argument reads the same. */
	writeText(path, "quiet " + devices + "\n");
	require(android::readKernelCommandLine(path, setsMultiplier) ==
		std::vector<std::string>{"quiet", devices});
	/* Any other multiplier is refused. */
	for (const char *bad : {"androidboot.hw_timeout_multiplier=3",
				"androidboot.hw_timeout_multiplier=50",
				"androidboot.hw_timeout_multiplier=05",
				"androidboot.hw_timeout_multiplier=",
				"androidboot.hw_timeout_multiplier=5x"}) {
		writeText(path, std::string{"quiet "} + bad + " " + devices + "\n");
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, setsMultiplier);
		});
		/* A launcher that does not set it passes the argument through. */
		const auto arguments =
			android::readKernelCommandLine(path, leavesMultiplier);
		require(std::find(arguments.begin(), arguments.end(),
				  std::string{bad}) != arguments.end());
	}
	writeText(path, "quiet androidboot.hw_timeout_multiplier=5 " + devices + "\n");
	require(android::readKernelCommandLine(path, leavesMultiplier) ==
		std::vector<std::string>{
			"quiet", "androidboot.hw_timeout_multiplier=5", devices});
	/* Dropping the argument leaves the rest of the checks in force. */
	writeText(path, "quiet androidboot.hw_timeout_multiplier=5\n");
	requireThrows([&] {
		(void)android::readKernelCommandLine(path, setsMultiplier);
	});
	writeText(path, "androidboot.hw_timeout_multiplier=5\n");
	requireThrows([&] {
		(void)android::readKernelCommandLine(path, setsMultiplier);
	});
}

/* A launcher that has the installation's serial number sets it itself, and the
 * release's own `umlandroid17` that every release published before then
 * carries is dropped for it. Any other serial in a release is refused by such
 * a launcher; a launcher with no serial of its own passes the release's
 * command line through as it is. */
void testSerialNumber(const fs::path &directory)
{
	require(android::serialPrefix == "androidboot.serialno=");
	require(android::serialArgument("0123456789abcdef") ==
		"androidboot.serialno=0123456789abcdef");
	require(android::legacySerialArgument ==
		std::string{android::serialPrefix} + "umlandroid17");

	const std::string devices{android::bootDevicesArgument};
	const auto path = directory / "serial-command-line.txt";
	constexpr std::array<std::string_view, 2> setsSerial{
		"androidboot.kernarium.adb_auth=", android::serialPrefix};
	constexpr std::array<std::string_view, 1> leavesSerial{
		"androidboot.kernarium.adb_auth="};

	/* The legacy argument is dropped, in whatever place it stands, and the
	 * others keep their order. */
	writeText(path, "quiet androidboot.serialno=umlandroid17 " + devices +
			" console=ttyS0\n");
	require(android::readKernelCommandLine(path, setsSerial) ==
		std::vector<std::string>{"quiet", devices, "console=ttyS0"});
	writeText(path, "androidboot.serialno=umlandroid17 quiet " + devices + "\n");
	require(android::readKernelCommandLine(path, setsSerial) ==
		std::vector<std::string>{"quiet", devices});
	/* A release without it reads the same. */
	writeText(path, "quiet " + devices + "\n");
	require(android::readKernelCommandLine(path, setsSerial) ==
		std::vector<std::string>{"quiet", devices});
	/* Any other serial is refused: the launcher fixes it. */
	for (const char *bad : {"androidboot.serialno=0123456789abcdef",
				"androidboot.serialno=umlandroid18",
				"androidboot.serialno=umlandroid17x",
				"androidboot.serialno=",
				"androidboot.serialno=UMLANDROID17"}) {
		writeText(path, std::string{"quiet "} + bad + " " + devices + "\n");
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, setsSerial);
		});
		/* A launcher with no serial of its own passes it through. */
		const auto arguments =
			android::readKernelCommandLine(path, leavesSerial);
		require(std::find(arguments.begin(), arguments.end(),
				  std::string{bad}) != arguments.end());
	}
	/* The release's command line is read as it is when the launcher has no
	 * serial: the legacy argument stays and boots the guest as before. */
	writeText(path, "quiet androidboot.serialno=umlandroid17 " + devices + "\n");
	require(android::readKernelCommandLine(path, leavesSerial) ==
		std::vector<std::string>{"quiet", "androidboot.serialno=umlandroid17",
					 devices});
	/* Dropping the argument leaves the rest of the checks in force. */
	writeText(path, "quiet androidboot.serialno=umlandroid17\n");
	requireThrows([&] {
		(void)android::readKernelCommandLine(path, setsSerial);
	});
	/* Android's reader removes quotes before it looks at an argument, so a
	 * quoted word sets a serial number like the plain one: the quoted legacy
	 * word is dropped and any other quoted serial is refused, whatever the
	 * quotes surround, and the guest gets exactly one serial. */
	for (const char *quotedLegacy :
	     {"\"androidboot.serialno=umlandroid17\"",
	      "androidboot.serialno=\"umlandroid17\"",
	      "'androidboot.serialno=umlandroid17'",
	      "androidboot.ser\"ial\"no=umlandroid17"}) {
		writeText(path, std::string{"quiet "} + quotedLegacy + " " + devices + "\n");
		require(android::readKernelCommandLine(path, setsSerial) ==
				std::vector<std::string>{"quiet", devices},
			std::string{"a quoted legacy serial stays: "} + quotedLegacy);
	}
	for (const char *quotedOther :
	     {"\"androidboot.serialno=0123456789abcdef\"",
	      "androidboot.serialno=\"0123456789abcdef\"",
	      "'androidboot.serialno=umlandroid18'",
	      "\"androidboot.serialno=umlandroid17x\""}) {
		writeText(path, std::string{"quiet "} + quotedOther + " " + devices + "\n");
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, setsSerial);
		});
	}
	/* A quoted group that holds a serial spans words: refused (see
	 * testQuotedGroups). */
	writeText(path, "\"quiet androidboot.serialno=0123456789abcdef\" " + devices + "\n");
	requireThrows([&] {
		(void)android::readKernelCommandLine(path, setsSerial);
	});
	/* A launcher with no serial of its own leaves quoted words as they are. */
	writeText(path, "\"androidboot.serialno=umlandroid17\" " + devices + "\n");
	require(android::readKernelCommandLine(path, leavesSerial) ==
		std::vector<std::string>{"\"androidboot.serialno=umlandroid17\"", devices});

	/* The words of a launcher argument, quotes removed. */
	require(android::withoutQuotes("a\"b'c") == "abc");
	require(android::carriesPrefix("androidboot.serialno=x", android::serialPrefix));
	require(android::carriesPrefix("quiet androidboot.serialno=x", android::serialPrefix));
	require(android::carriesPrefix("\"androidboot.serialno=x\"", android::serialPrefix));
	require(android::carriesPrefix("a\tb\n'androidboot.serialno=x'", android::serialPrefix));
	require(android::carriesPrefix("androidboot.ser\"ialno=x", android::serialPrefix));
	require(!android::carriesPrefix("", android::serialPrefix));
	require(!android::carriesPrefix("quiet", android::serialPrefix));
	require(!android::carriesPrefix("--android-serialno=ABCDEF", android::serialPrefix));
	require(!android::carriesPrefix("xandroidboot.serialno=x", android::serialPrefix));
	require(!android::carriesPrefix("androidboot.serialnumber=x", android::serialPrefix));

	/* The launchers' own multiplier rule is unaffected. */
	writeText(path, "androidboot.serialno=umlandroid17 "
			"androidboot.hw_timeout_multiplier=5 " + devices + "\n");
	constexpr std::array<std::string_view, 2> setsBoth{
		android::serialPrefix, android::hwTimeoutMultiplierPrefix};
	require(android::readKernelCommandLine(path, setsBoth) ==
		std::vector<std::string>{devices});
}

/* The loop of the pinned Android fs_mgr_boot_config.cpp
 * ImportKernelCmdlineFromString: a word ends at a space outside a `"` group, and
 * the quote characters are removed from it. */
std::map<std::string, std::string> importedProperties(const std::string &line)
{
	std::map<std::string, std::string> properties;
	std::size_t base = 0;
	while (true) {
		auto found = base;
		while ((found = line.find_first_of(" \"", found)) !=
			       std::string::npos &&
		       line[found] == '"') {
			if ((found = line.find('"', found + 1)) ==
			    std::string::npos)
				break;
			++found;
		}
		std::string piece = line.substr(base, found - base);
		piece.erase(std::remove(piece.begin(), piece.end(), '"'),
			    piece.end());
		const auto equals = piece.find('=');
		if (equals != std::string::npos)
			properties.emplace(piece.substr(0, equals),
					   piece.substr(equals + 1));
		if (found == std::string::npos)
			break;
		base = found + 1;
	}
	return properties;
}

/* The release's arguments as the launcher joins them with its own. */
std::map<std::string, std::string> importedAfterLauncher(
	const std::vector<std::string> &release)
{
	std::string line;
	for (const auto &argument : release)
		line += argument + ' ';
	line += android::lcdDensityArgument(240) + ' ' +
		android::hwTimeoutMultiplierArgument(8) + ' ' +
		android::serialArgument("0123456789abcdef");
	return importedProperties(line);
}

/* A quoted group of a release's command line is one argument to Android and
 * several words to the reader that drops the legacy serial and multiplier, and
 * the launcher's own arguments come after the release's: a group that spans
 * words, or a quote that never closes, is refused before anything is dropped,
 * and what is accepted reaches Android as the launcher wrote it. */
void testQuotedGroups(const fs::path &directory)
{
	const std::string devices{android::bootDevicesArgument};
	const auto path = directory / "quoted-groups.txt";
	constexpr std::array<std::string_view, 3> setsAll{
		"androidboot.kernarium.adb_auth=", android::serialPrefix,
		android::hwTimeoutMultiplierPrefix};
	constexpr std::array<std::string_view, 1> leavesAll{
		"androidboot.kernarium.adb_auth="};
	const std::string legacySerial{android::legacySerialArgument};
	const std::string legacyMultiplier{
		android::legacyHwTimeoutMultiplierArgument};

	for (const std::string &group : std::vector<std::string>{
		     "\"quiet " + legacySerial + "\"",
		     "\"" + legacySerial + " quiet\"",
		     "\"quiet " + legacyMultiplier + "\"",
		     "\"" + legacyMultiplier + " quiet\"",
		     "quiet \"" + legacySerial + " extra\"",
		     "androidboot.x=\"a b\"",
		     "\"quiet\tconsole=ttyS0\"",
		     "\"quiet\nconsole=ttyS0\"",
		     "\"quiet " + legacySerial,
		     "quiet " + legacySerial + "\"",
		     "androidboot.ser\"ialno=umlandroid17",
		     "\"a\" \"b"}) {
		writeText(path, group + " " + devices + "\n");
		/* Refused for every launcher: its own arguments follow the open quote. */
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, setsAll);
		});
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, leavesAll);
		});
	}

	/* The quote may be anywhere in the file, even after the arguments the
	 * launcher would have dropped, and the file need not end in a newline. */
	for (const std::string &tail : std::vector<std::string>{
		     " \"quiet\n", " \"quiet", " quiet\"", " a\"b\"c\"",
		     " androidboot.x=\"y"}) {
		writeText(path, legacySerial + " " + devices + tail);
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, setsAll);
		});
	}

	/* A quote that opens and closes inside one word is a group of no words
	 * to span: the legacy arguments in it are dropped and Android reads the
	 * launcher's own as written, whatever the quotes surround. */
	for (const std::string &whole : std::vector<std::string>{
		     "\"" + legacySerial + "\"",
		     "androidboot.serialno=\"umlandroid17\"",
		     "androidboot.ser\"ial\"no=umlandroid17"}) {
		writeText(path, "quiet " + whole + " " + legacyMultiplier +
				" \"console=ttyS0\" " + devices + "\n");
		const auto arguments = android::readKernelCommandLine(path, setsAll);
		require(arguments ==
			std::vector<std::string>{"quiet", "\"console=ttyS0\"", devices});
		const auto properties = importedAfterLauncher(arguments);
		require(properties.at("androidboot.serialno") == "0123456789abcdef");
		require(properties.at("androidboot.lcd_density") == "240");
		require(properties.at("androidboot.hw_timeout_multiplier") == "3");
		require(properties.at("console") == "ttyS0");
	}
	/* A launcher that keeps the release's own multiplier passes it through
	 * whole, quotes included, and the guest gets it. */
	writeText(path, "\"" + legacyMultiplier + "\" " + devices + "\n");
	const auto kept = android::readKernelCommandLine(path, leavesAll);
	require(kept == std::vector<std::string>{"\"" + legacyMultiplier + "\"", devices});
	require(importedProperties(kept[0] + " " + kept[1]).at(
			"androidboot.hw_timeout_multiplier") == "5");
}

/* Both launchers reserve the hardware SKU that tells the guest what the host
 * has (the iOS runtime's KernariumHostFeatures.hpp, and the standalone
 * launcher, which passes a host's own argument through) and the vendor SKU
 * that names the camera layout the host composed
 * (camera/camera_layout.hpp), so a release cannot state either. The product
 * axis stays the release's. */
void testHostFeatureArguments(const fs::path &directory)
{
	constexpr std::array<std::string_view, 3> launcherPrefixes{
		"androidboot.kernarium.adb_auth=",
		"androidboot.product.hardware.sku=",
		"androidboot.product.vendor.sku="};
	const std::string devices{android::bootDevicesArgument};
	const auto path = directory / "feature-command-line.txt";
	for (const char *bad : {"androidboot.product.hardware.sku=haptics",
				"androidboot.product.hardware.sku=",
				"androidboot.product.hardware.sku=other",
				"androidboot.product.vendor.sku=camera",
				"androidboot.product.vendor.sku="}) {
		writeText(path, std::string{"quiet "} + bad + " " + devices + "\n");
		requireThrows([&] {
			(void)android::readKernelCommandLine(path, launcherPrefixes);
		});
	}
	writeText(path, "quiet androidboot.hardware.sku=camera "
			"androidboot.product.hardware.skus=1 " + devices + "\n");
	require(android::readKernelCommandLine(path, launcherPrefixes) ==
		std::vector<std::string>{"quiet", "androidboot.hardware.sku=camera",
					 "androidboot.product.hardware.skus=1", devices});
}

/* Both launchers read the release's kernel command line through one
 * tokenizer and refuse an argument that they set themselves. */
void testKernelCommandLine(const fs::path &directory)
{
	constexpr std::array<std::string_view, 1> launcherPrefixes{
		"androidboot.kernarium.adb_auth="};
	const std::string devices{android::bootDevicesArgument};
	const auto path = directory / "kernel-command-line.txt";
	writeText(path, "  console=ttyS0\tandroidboot.hardware=kernarium\n\nquiet \n" +
			devices + "\n");
	require(android::readKernelCommandLine(path, launcherPrefixes) ==
		std::vector<std::string>{"console=ttyS0", "androidboot.hardware=kernarium",
					 "quiet", devices});

	writeText(path, "quiet androidboot.kernarium.adb_auth=0 " + devices + "\n");
	requireThrows([&] { (void)android::readKernelCommandLine(path, launcherPrefixes); });
	writeText(path, " \n\t\n");
	requireThrows([&] { (void)android::readKernelCommandLine(path, launcherPrefixes); });
	const auto link = directory / "kernel-command-line-link.txt";
	writeText(path, "quiet " + devices + "\n");
	fs::create_symlink(path, link);
	requireThrows([&] { (void)android::readKernelCommandLine(link, launcherPrefixes); });
	requireThrows([&] {
		(void)android::readKernelCommandLine(directory / "missing.txt", launcherPrefixes);
	});
}

/* A disk is named in a UBD specification. */
void testUbdPath()
{
	requireThrows([] { android::validateUbdPath("", "disk"); });
	requireThrows([] { android::validateUbdPath("/a,b.img", "disk"); });
	requireThrows([] { android::validateUbdPath("/a:b.img", "disk"); });
	/* The kernel splits its command line at whitespace, byte 0xa0 included,
	 * and reads a double quote as quoting. */
	requireThrows([] { android::validateUbdPath("/My Disk.img", "disk"); });
	requireThrows([] { android::validateUbdPath("/a\tb.img", "disk"); });
	requireThrows([] { android::validateUbdPath("/a\nb.img", "disk"); });
	requireThrows([] { android::validateUbdPath("/a\"b.img", "disk"); });
	requireThrows([] { android::validateUbdPath("/\xe8\xa0\x80.img", "disk"); });
	android::validateUbdPath("/a/b.img", "disk");
	android::validateUbdPath("/a/\xe6\x97\xa5.img", "disk");
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s FIXTURE_DIRECTORY\n", argv[0]);
		return 2;
	}
	const auto fixture = openFixture(argv[1]);
	/* An uncaught failure leaves the directory, which names this test. */
	const uml::testing::ScratchSubdirectory scratch{
		"uml-android-boot-tests.XXXXXX"};
	const auto &directory = scratch.path();
	const auto fresh = [&](const char *name) {
		const auto path = directory / name;
		fs::create_directory(path);
		return path;
	};

	testGuid();
	testLayoutParsing(fixture);
	testLayoutRefusals(fixture);
	testSystemDisk(fixture, fresh("system"));
	testDataDiskCreate(fixture, fresh("create"));
	testDataDiskValidation(fixture, fresh("validate"));
	testDataDiskGrow(fixture, fresh("grow"));
	testStorageBoot(fixture, fresh("boot"));
	testBootloaderCommandIsConsumed(fixture, fresh("consume"));
	testRestartSelectsAutomaticMode(fixture, fresh("restart"));
	testRootBootImage(fixture, fresh("root-image"));
	testStorageIsExact(fixture, fresh("exact"));
	testLcdDensity(fresh("density"));
	testHwTimeoutMultiplier(fresh("multiplier"));
	testSerialNumber(fresh("serial"));
	testQuotedGroups(fresh("quoted-groups"));
	testHostFeatureArguments(fresh("feature-arguments"));
	testKernelCommandLine(fresh("command-line"));
	testUbdPath();
	return 0;
}
