// SPDX-License-Identifier: GPL-2.0-or-later
// The colour data a Bayer still teaches, kept per host model and lens: it
// survives a new store over the same directory, a damaged file is ignored, a
// save replaces the file whole, and a store with no directory keeps memory only.

#include "calibration_store.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace uml::camera;
using namespace uml::platform;
using uml::testing::require;

namespace {

CameraColorData sample(double seed)
{
    CameraColorData data;
    data.bayer = BayerOrder::gbrg;
    data.blackLevel = 64;
    data.whiteLevel = 1023;
    data.rawWidth = 4032;
    data.rawHeight = 3024;
    data.illuminant1 = 17;
    data.illuminant2 = 21;
    // The format the camera ran in when the data was learned.
    data.anchorWidth = 1920 + std::uint32_t(seed);
    data.anchorHeight = 1440;
    for (std::size_t index = 0; index < 9; ++index) {
        data.colorMatrix1[index] = seed + double(index) * 0.125;
        data.colorMatrix2[index] = seed - double(index) * 0.0625;
        data.forwardMatrix1[index] = seed * 2 + double(index);
        data.forwardMatrix2[index] = seed * 3 + double(index);
    }
    data.focalLengthMillimetres = 6.86;
    data.intrinsics = {3100.5, 3101.5, 2016.0, 1512.0};
    return data;
}

// A calibration file as the store writes it, made here from its description (the test's own reference, not the store's
// code) so that its check word is valid: the eight bytes of the version, the Bayer order, the integer fields (the
// anchor's size last, in the current version only), the four matrices, the focal length and the intrinsics, then the CRC-32.
std::vector<std::uint8_t> fileOf(const CameraColorData& data, const std::string& version, bool withAnchor)
{
    std::vector<std::uint8_t> out(version.begin(), version.end());
    const auto put = [&out](const auto& value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(value));
    };
    put(std::uint32_t(data.bayer));
    for (const auto value : {data.blackLevel, data.whiteLevel, data.rawWidth, data.rawHeight, data.illuminant1, data.illuminant2})
        put(std::uint32_t(value));
    if (withAnchor) {
        put(std::uint32_t(data.anchorWidth));
        put(std::uint32_t(data.anchorHeight));
    }
    for (const auto* matrix : {&data.colorMatrix1, &data.colorMatrix2, &data.forwardMatrix1, &data.forwardMatrix2})
        for (const auto value : *matrix) put(double(value));
    put(double(data.focalLengthMillimetres));
    for (const auto value : data.intrinsics) put(double(value));
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : out) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    put(std::uint32_t(~crc));
    return out;
}

CameraInfo lens(CameraPosition position, CameraLensKind kind)
{
    CameraInfo info;
    info.position = position;
    info.lens = kind;
    return info;
}

void testKeys()
{
    require(CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)) !=
                CalibrationStore::lensKey(lens(CameraPosition::front, CameraLensKind::wide)),
            "the same lens on another side is another key");
    require(CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)) !=
                CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::ultraWide)),
            "another lens is another key");
    require(CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)) ==
                CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)),
            "a key is stable");
}

void testMemoryOnly()
{
    CalibrationStore store;
    require(!store.load("Model1,1", "back-wide"), "nothing is known at first");
    require(store.save("Model1,1", "back-wide", sample(1.0)), "memory always saves");
    require(store.load("Model1,1", "back-wide") == sample(1.0), "a saved value comes back");
    require(!store.load("Model1,2", "back-wide"), "another model has nothing");
    require(!store.load("Model1,1", "front-wide"), "another lens has nothing");
}

void testPersistence()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    const auto root = directory.path() / "store";
    {
        CalibrationStore store(root);
        require(store.save("Model/1,1", "back wide", sample(2.0)), "the first save creates the directory");
        require(store.save("Model/1,1", "front-wide", sample(3.0)), "a second lens");
    }
    CalibrationStore reopened(root);
    require(reopened.load("Model/1,1", "back wide") == sample(2.0), "a new store reads the first lens");
    require(reopened.load("Model/1,1", "front-wide") == sample(3.0), "and the second");
    require(!reopened.load("Model/1,1", "back-ultrawide"), "and nothing it was not given");

    // A save replaces the file whole and leaves no temporary file behind.
    require(reopened.save("Model/1,1", "back wide", sample(4.0)), "replacing a value");
    CalibrationStore third(root);
    require(third.load("Model/1,1", "back wide") == sample(4.0), "the replacement is what a later store reads");
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        ++files;
        require(entry.path().extension() == ".cal", "only calibration files are left");
    }
    require(files == 2, "one file a lens");
}

void testDamagedFile()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    const auto root = directory.path();
    {
        CalibrationStore store(root);
        require(store.save("M", "lens", sample(5.0)), "save");
    }
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(root)) file = entry.path();
    require(!file.empty(), "a file was written");
    // Flip one byte: the check word no longer matches.
    {
        std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(20);
        char byte = 0;
        stream.read(&byte, 1);
        stream.seekp(20);
        byte = char(byte ^ 0x5a);
        stream.write(&byte, 1);
    }
    CalibrationStore damaged(root);
    require(!damaged.load("M", "lens"), "a file that fails its check is ignored");
    // Cut short.
    std::filesystem::resize_file(file, 12);
    CalibrationStore shortened(root);
    require(!shortened.load("M", "lens"), "a short file is ignored");
    // An empty file, then a good save over it.
    std::filesystem::resize_file(file, 0);
    CalibrationStore empty(root);
    require(!empty.load("M", "lens"), "an empty file is ignored");
    require(empty.save("M", "lens", sample(6.0)), "a save over a damaged file");
    CalibrationStore after(root);
    require(after.load("M", "lens") == sample(6.0), "and it reads back");
}

// The anchor's size is stored with the data, and a file of the earlier version (which has none) is not read: the
// lens is learned again in its anchor.
void testTheAnchorIsStoredAndAnEarlierVersionIsIgnored()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    const auto root = directory.path();
    {
        CalibrationStore store(root);
        require(store.save("M", "lens", sample(8.0)), "save");
    }
    {
        CalibrationStore reopened(root);
        const auto loaded = reopened.load("M", "lens");
        require(loaded && loaded->anchorWidth == 1928 && loaded->anchorHeight == 1440, "the anchor's size comes back");
    }
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(root)) file = entry.path();
    {
        std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(7);
        stream.put('1');
    }
    CalibrationStore earlier(root);
    require(!earlier.load("M", "lens"), "a file of the version that stored no anchor is ignored");
}

// A file whose check word is valid is rejected for its version alone: the current layout under the earlier version's
// name, and the earlier version's own layout (which has no anchor), each with the CRC-32 of what is in it. Neither is
// the damage the check word catches, so only the store's own comparison of the version stops them.
void testAFileOfAnotherVersionWithAValidCheckWordIsIgnored()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    const auto root = directory.path();
    const CameraColorData data = sample(9.0);
    {
        CalibrationStore store(root);
        require(store.save("M", "lens", data), "save");
    }
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(root)) file = entry.path();
    const auto write = [&](const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    };
    write(fileOf(data, "UMLCAL02", true));
    {
        CalibrationStore reference(root);
        require(reference.load("M", "lens") == data, "the test's own file of the current version reads back, so its check word is valid");
    }
    write(fileOf(data, "UMLCAL01", true));
    {
        CalibrationStore version(root);
        require(!version.load("M", "lens"), "the current layout under another version's name is ignored");
    }
    write(fileOf(data, "UMLCAL01", false));
    {
        CalibrationStore earlier(root);
        require(!earlier.load("M", "lens"), "a file of the earlier version, in its own layout, is ignored");
    }
    write(fileOf(data, "UMLCAL03", true));
    {
        CalibrationStore later(root);
        require(!later.load("M", "lens"), "and so is one of a later version");
    }
}

void testUnwritableDirectory()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    // A file where the directory should be.
    const auto blocker = directory.path() / "blocker";
    std::ofstream(blocker).put('x');
    CalibrationStore store(blocker / "inside");
    require(!store.save("M", "lens", sample(7.0)), "an unwritable directory reports the failure");
    require(store.load("M", "lens") == sample(7.0), "but the value is kept in memory");
}

} // namespace

int main()
{
    testKeys();
    testMemoryOnly();
    testPersistence();
    testDamagedFile();
    testTheAnchorIsStoredAndAnEarlierVersionIsIgnored();
    testAFileOfAnotherVersionWithAValidCheckWordIsIgnored();
    testUnwritableDirectory();
    return 0;
}
