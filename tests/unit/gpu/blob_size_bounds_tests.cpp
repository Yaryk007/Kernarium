// SPDX-License-Identifier: GPL-2.0-or-later

// A guest chooses the size of every host-visible blob it creates, and the
// renderer maps that many bytes of the shared memory behind it. A blob larger
// than that memory must be refused where it is created, because the host
// faults on the first access past the end of the file behind it, and the
// render server is a thread of the host process. The contract under test:
//
//  - a Venus blob larger than its memory is refused, whether the memory's
//    pages are POSIX shared memory (the macOS default) or an unlinked file
//    (iOS, where shm_open is denied, and VIRGL_ANON_FILE_UNLINKED=1);
//  - the same memory can then be exported at its own size;
//  - a command that names the refused resource id does not end the host
//    process;
//  - a refused blob leaves nothing mapped and no pages held, so a guest
//    cannot grow the host's address space or residency by asking again; and
//  - a GL blob larger than the shared pages behind a persistent coherent
//    buffer is refused, and the buffer can then be taken at its own size; and
//  - the shared pages are whole host pages, so a blob of memory that is not a
//    whole number of pages may ask for the bytes it allocated and for the
//    pages it was rounded up to, and is refused one byte past them.
//
// The Venus cases run in a child each, because a renderer without the bound
// ends the process that holds it and the backing is chosen once per process.
// The GL case is the test gpu-blob-size-bounds-gl: it needs the host's pinned
// memory path, and a renderer without it reports the test skipped. A case that
// is skipped makes its test report skipped, whatever the others did, unless
// one failed.

#include "gpu_test_support.hpp"
#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "venus_wire.hpp"
#include "virgl_renderer.hpp"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using uml::gpu::VirglRenderer;
using uml::gpu::testing::launchChild;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::waitForChild;
using uml::testing::require;

namespace protocol = uml::gpu::protocol;
namespace venus = uml::gpu::testing::venus;

// The host page is the unit the render server maps and the device exports in.
constexpr std::uint64_t pageBytes = 16384;
constexpr std::uint64_t oversizedBytes = 4 * pageBytes;
// Memory that is not a whole number of host pages: the renderer backs it with
// two.
constexpr std::uint64_t unalignedBytes = pageBytes + 3616;
constexpr std::uint32_t cmdFreeMemory = 22;
constexpr std::uint32_t cmdExecuteCommandStreams = 180;

void setBacking(bool unlinked)
{
    if (unlinked)
        require(::setenv("VIRGL_ANON_FILE_UNLINKED", "1", 1) == 0);
    else
        require(::unsetenv("VIRGL_ANON_FILE_UNLINKED") == 0);
}

[[nodiscard]] std::uint64_t virtualBytes()
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    require(::task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS,
        "the task figures could not be read");
    return info.virtual_size;
}

// How many mappings of exactly `length` bytes the process holds.
[[nodiscard]] int regionsOf(std::uint64_t length)
{
    int count = 0;
    mach_vm_address_t address = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info {};
        mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (::mach_vm_region(mach_task_self(), &address, &size,
                VM_REGION_BASIC_INFO_64,
                reinterpret_cast<vm_region_info_t>(&info), &infoCount,
                &object) != KERN_SUCCESS) {
            break;
        }
        if (size == length)
            ++count;
        address += size;
    }
    return count;
}

// A Venus guest: one context with an instance and a device, and the index of
// the memory type whose host-visible memory the renderer backs with shared
// pages.
struct VenusGuest {
    static constexpr std::uint32_t ctxId = 1;
    static constexpr std::uint64_t instanceId = 1;
    static constexpr std::uint64_t physDevId = 2;
    static constexpr std::uint64_t deviceId = 3;

    VirglRenderer& renderer;
    std::uint32_t hostTypeIndex {};
    std::uint64_t fence {};

    explicit VenusGuest(VirglRenderer& r) : renderer(r)
    {
        require(renderer.createContext(ctxId, protocol::venusCapsetId,
                    "blob-bounds") == 0,
            "the Venus context could not be created");
        venus::Stream setup;
        venus::encodeInstanceAndDevice(
            setup, VK_API_VERSION_1_2, instanceId, physDevId, deviceId);
        require(renderer.submit(ctxId, setup.bytes()) == 0,
            "the instance and device could not be created");
        sync();
        bool found = false;
        for (std::uint32_t typeIndex = 0; typeIndex < 8 && !found; ++typeIndex) {
            const std::uint64_t memId = 0x1000 + typeIndex;
            allocate(memId, pageBytes, typeIndex);
            const std::uint32_t resId = 0x100 + typeIndex;
            if (createBlob(resId, memId, pageBytes) == 0) {
                int fd = -1;
                std::uint32_t fdType = 0;
                require(renderer.exportResourceBlob(resId, fdType, fd) == 0 &&
                        fd >= 0,
                    "a host-visible blob could not be exported");
                ::close(fd);
                hostTypeIndex = typeIndex;
                found = true;
                renderer.unrefResource(resId);
            }
            freeMemory(memId);
            sync();
        }
        require(found, "the device has no host-visible memory type");
    }

    void sync()
    {
        (void)renderer.createFenceAndWait(ctxId, 0, ++fence, nullptr);
    }

    void allocate(std::uint64_t memId, std::uint64_t size, std::uint32_t typeIndex)
    {
        venus::Stream s;
        s.command(venus::allocateMemory);
        s.u64(deviceId);
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        s.u64(0);
        s.u64(size);
        s.u32(typeIndex);
        s.ptr(false);
        s.ptr(true);
        s.u64(memId);
        require(renderer.submit(ctxId, s.bytes()) == 0,
            "a memory allocation was not accepted");
    }

    void freeMemory(std::uint64_t memId)
    {
        venus::Stream s;
        s.u32(cmdFreeMemory);
        s.u32(0);
        s.u64(deviceId);
        s.u64(memId);
        s.ptr(false);
        require(renderer.submit(ctxId, s.bytes()) == 0,
            "a memory release was not accepted");
    }

    [[nodiscard]] int createBlob(
        std::uint32_t resId, std::uint64_t memId, std::uint64_t size)
    {
        return renderer.createResourceBlob(VirglRenderer::ResourceBlob {
            .resourceId = resId,
            .contextId = ctxId,
            .memory = protocol::blobMemoryHost3d,
            .flags = protocol::blobFlagMappable,
            .blobId = memId,
            .size = size,
        });
    }

    // vkExecuteCommandStreamsMESA over `size` bytes of the resource at
    // `offset`: the render server reads the stream from the resource.
    [[nodiscard]] int executeStream(
        std::uint32_t resId, std::uint64_t offset, std::uint64_t size)
    {
        venus::Stream s;
        s.u32(cmdExecuteCommandStreams);
        s.u32(0);
        s.u32(1);
        s.u64(1);
        s.u32(resId);
        s.u64(offset);
        s.u64(size);
        s.u64(0);
        s.u32(0);
        s.u64(0);
        s.u32(0);
        return renderer.submit(ctxId, s.bytes());
    }
};

// The size of the file behind an exported blob, which the guest's mapping of
// it must not exceed.
[[nodiscard]] std::uint64_t exportedBytes(
    VirglRenderer& renderer, std::uint32_t resourceId)
{
    int fd = -1;
    std::uint32_t fdType = 0;
    require(renderer.exportResourceBlob(resourceId, fdType, fd) == 0 && fd >= 0,
        "the blob could not be exported");
    struct stat status {};
    require(::fstat(fd, &status) == 0, "the exported file could not be examined");
    ::close(fd);
    return static_cast<std::uint64_t>(status.st_size);
}

// A blob larger than its memory is refused, and the memory is still there to
// export at its own size.
void refuseOversizedVenusBlob(bool unlinked)
{
    setBacking(unlinked);
    VirglRenderer renderer;
    VenusGuest guest {renderer};

    constexpr std::uint64_t memId = 0x2000;
    constexpr std::uint32_t oversizedRes = 200;
    constexpr std::uint32_t ownRes = 201;
    guest.allocate(memId, pageBytes, guest.hostTypeIndex);
    guest.sync();

    require(guest.createBlob(oversizedRes, memId, oversizedBytes) != 0,
        "a blob larger than its memory was accepted");
    require(guest.createBlob(ownRes, memId, pageBytes) == 0,
        "the memory could not be exported at its own size after a refusal");
    require(exportedBytes(renderer, ownRes) >= pageBytes,
        "the file behind a blob is shorter than the blob");
    renderer.unrefResource(ownRes);
    renderer.destroyContext(VenusGuest::ctxId);
}

// A command that names the id of a refused blob is the guest's error and not
// the host's: where the render server kept the resource, this read of it past
// the end of its file ended the host process.
void useRefusedVenusBlob(bool unlinked)
{
    setBacking(unlinked);
    VirglRenderer renderer;
    VenusGuest guest {renderer};

    constexpr std::uint64_t memId = 0x2100;
    constexpr std::uint32_t oversizedRes = 300;
    guest.allocate(memId, pageBytes, guest.hostTypeIndex);
    guest.sync();
    require(guest.createBlob(oversizedRes, memId, oversizedBytes) != 0,
        "a blob larger than its memory was accepted");
    (void)guest.executeStream(oversizedRes, pageBytes, 64);
    guest.sync();
    renderer.destroyContext(VenusGuest::ctxId);
}

// Memory that is not a whole number of host pages is backed by whole pages.
// A blob may ask for the bytes the guest allocated and for the pages they
// were rounded up to, and not for one byte more. A memory backs one resource
// at a time, so each attempt has a memory of its own.
void boundVenusBlobByItsPages(bool unlinked)
{
    setBacking(unlinked);
    VirglRenderer renderer;
    VenusGuest guest {renderer};

    std::uint64_t memId = 0x2200;
    std::uint32_t resId = 500;
    const auto attempt = [&](std::uint64_t blobBytes) {
        ++memId;
        guest.allocate(memId, unalignedBytes, guest.hostTypeIndex);
        guest.sync();
        return guest.createBlob(++resId, memId, blobBytes);
    };

    require(attempt(unalignedBytes) == 0,
        "a blob of the memory's own size was refused");
    const std::uint64_t paddedBytes = exportedBytes(renderer, resId);
    require(paddedBytes > unalignedBytes && paddedBytes % pageBytes == 0,
        "the file behind a blob is not the memory rounded up to host pages");
    renderer.unrefResource(resId);

    require(attempt(paddedBytes) == 0,
        "a blob of the pages behind its memory was refused");
    renderer.unrefResource(resId);

    require(attempt(paddedBytes + 1) != 0,
        "a blob one byte past the pages behind its memory was accepted");
    require(guest.createBlob(++resId, memId, unalignedBytes) == 0,
        "the memory could not be exported at its own size after a refusal");
    require(exportedBytes(renderer, resId) >= unalignedBytes,
        "the file behind a blob is shorter than the blob");
    renderer.unrefResource(resId);
    renderer.destroyContext(VenusGuest::ctxId);
}

// Refusals leave nothing mapped: a guest that asks for oversized blobs of
// memory it writes and releases gains no address space and no pages.
void refuseOversizedVenusBlobsWithoutRetention()
{
    setBacking(true);
    VirglRenderer renderer;
    VenusGuest guest {renderer};

    constexpr std::uint64_t memoryBytes = 16ULL << 20;
    constexpr int attempts = 8;
    const int regionsBefore = regionsOf(memoryBytes * 2);
    const std::uint64_t virtualBefore = virtualBytes();
    for (int i = 0; i < attempts; ++i) {
        const std::uint64_t memId = 0x3000 + static_cast<std::uint64_t>(i);
        const std::uint32_t ownRes = 400 + static_cast<std::uint32_t>(i) * 2;
        const std::uint32_t oversizedRes = ownRes + 1;
        guest.allocate(memId, memoryBytes, guest.hostTypeIndex);
        guest.sync();
        // The guest writes its memory through a blob of the right size...
        require(guest.createBlob(ownRes, memId, memoryBytes) == 0,
            "a blob of the memory's own size was refused");
        int fd = -1;
        std::uint32_t fdType = 0;
        require(renderer.exportResourceBlob(ownRes, fdType, fd) == 0 && fd >= 0,
            "the blob could not be exported");
        void* mapping = ::mmap(nullptr, memoryBytes, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, 0);
        require(mapping != MAP_FAILED, "the blob could not be mapped");
        std::memset(mapping, 0xa5, memoryBytes);
        ::munmap(mapping, memoryBytes);
        ::close(fd);
        renderer.unrefResource(ownRes);
        guest.sync();
        // ... asks for a larger one, and releases the memory.
        require(guest.createBlob(oversizedRes, memId, memoryBytes * 2) != 0,
            "a blob larger than its memory was accepted");
        guest.freeMemory(memId);
        guest.sync();
    }
    const int regionsAfter = regionsOf(memoryBytes * 2);
    const std::uint64_t grown = virtualBytes() > virtualBefore
        ? virtualBytes() - virtualBefore
        : 0;
    std::fprintf(stderr,
        "after %d refusals: %d kept mappings of %llu MiB, address space grew "
        "by %llu MiB\n",
        attempts, regionsAfter - regionsBefore,
        static_cast<unsigned long long>((memoryBytes * 2) >> 20),
        static_cast<unsigned long long>(grown >> 20));
    require(regionsAfter <= regionsBefore,
        "a refused blob left a mapping of its size in the host");
    require(grown < (memoryBytes * attempts) / 2,
        "refused blobs grew the host's address space");
    renderer.destroyContext(VenusGuest::ctxId);
}

// PIPE_RESOURCE_CREATE of a buffer of `bytes` bytes with persistent, coherent
// storage that the blob id names.
[[nodiscard]] int createCoherentBuffer(VirglRenderer& renderer,
    std::uint32_t contextId, std::uint32_t blobId,
    std::uint64_t bytes = pageBytes)
{
    constexpr std::uint32_t commandPipeResourceCreate = 48;
    constexpr std::uint32_t pipeBuffer = 0;
    constexpr std::uint32_t virglFormatR8Unorm = 64;
    constexpr std::uint32_t virglBindVertexBuffer = 1U << 4U;
    constexpr std::uint32_t mapPersistentCoherent = (1U << 1U) | (1U << 2U);
    const std::vector<std::uint32_t> words {
        (11U << 16U) | commandPipeResourceCreate, pipeBuffer,
        virglFormatR8Unorm, virglBindVertexBuffer,
        static_cast<std::uint32_t>(bytes), 1, 1, 1, 0, 0,
        mapPersistentCoherent, blobId,
    };
    return renderer.submit(contextId,
        {reinterpret_cast<const std::uint8_t*>(words.data()),
            words.size() * sizeof(std::uint32_t)});
}

[[nodiscard]] int createGlBlob(VirglRenderer& renderer,
    std::uint32_t contextId, std::uint32_t resourceId, std::uint32_t blobId,
    std::uint64_t size)
{
    return renderer.createResourceBlob(VirglRenderer::ResourceBlob {
        .resourceId = resourceId,
        .contextId = contextId,
        .memory = protocol::blobMemoryHost3d,
        .flags = protocol::blobFlagMappable,
        .blobId = blobId,
        .size = size,
    });
}

// A persistent coherent GL buffer is backed by shared host pages, which the
// guest maps whole; a blob of it that is larger than the pages is refused,
// and the buffer stays for a blob of its own size. A renderer whose host
// driver has no pinned memory backs the buffer with nothing a guest can map:
// that is found out on a buffer of its own first, and the case is skipped.
void refuseOversizedGlBlob()
{
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        std::exit(skipWithoutRenderer(error));
    }
    constexpr std::uint32_t contextId = 1;
    require(renderer->createContext(contextId, protocol::virgl2CapsetId,
                "blob-bounds") == 0,
        "the GL context could not be created");

    constexpr std::uint32_t probeBlobId = 2;
    constexpr std::uint32_t probeResource = 11;
    bool sharedPages = createCoherentBuffer(*renderer, contextId, probeBlobId) == 0 &&
        createGlBlob(*renderer, contextId, probeResource, probeBlobId,
            pageBytes) == 0;
    if (sharedPages) {
        int fd = -1;
        std::uint32_t fdType = 0;
        sharedPages = renderer->exportResourceBlob(probeResource, fdType, fd) == 0 &&
            fd >= 0 && fdType == VirglRenderer::sharedMemoryBlobDescriptor;
        if (fd >= 0)
            ::close(fd);
        renderer->unrefResource(probeResource);
    }
    if (!sharedPages) {
        std::fputs("skipping: this renderer backs a coherent buffer with "
                   "nothing a guest can map (no pinned host memory)\n",
            stderr);
        std::exit(77);
    }

    constexpr std::uint32_t blobId = 1;
    constexpr std::uint32_t resourceId = 10;
    require(createCoherentBuffer(*renderer, contextId, blobId) == 0,
        "the persistent buffer could not be created");
    require(createGlBlob(*renderer, contextId, resourceId, blobId,
                oversizedBytes) != 0,
        "a blob larger than its coherent buffer was accepted");
    require(createGlBlob(*renderer, contextId, resourceId, blobId,
                pageBytes) == 0,
        "the buffer could not be taken at its own size after a refusal");
    require(exportedBytes(*renderer, resourceId) >= pageBytes,
        "the pages behind a blob are shorter than the blob");
    renderer->unrefResource(resourceId);

    // A buffer that is not a whole number of host pages is backed by whole
    // pages: a blob may ask for its bytes and for the pages it was rounded up
    // to, and not for one byte more. A blob takes its buffer, so each attempt
    // has a buffer of its own.
    std::uint32_t paddedBlobId = 20;
    std::uint32_t paddedResource = 30;
    const auto attempt = [&](std::uint64_t blobBytes) {
        ++paddedBlobId;
        require(createCoherentBuffer(
                    *renderer, contextId, paddedBlobId, unalignedBytes) == 0,
            "the unaligned persistent buffer could not be created");
        return createGlBlob(
            *renderer, contextId, ++paddedResource, paddedBlobId, blobBytes);
    };
    require(attempt(unalignedBytes) == 0,
        "a blob of the buffer's own size was refused");
    const std::uint64_t paddedBytes = exportedBytes(*renderer, paddedResource);
    require(paddedBytes > unalignedBytes && paddedBytes % pageBytes == 0,
        "the pages behind a buffer are not the buffer rounded up to host pages");
    renderer->unrefResource(paddedResource);

    require(attempt(paddedBytes) == 0,
        "a blob of the pages behind its buffer was refused");
    renderer->unrefResource(paddedResource);

    require(attempt(paddedBytes + 1) != 0,
        "a blob one byte past the pages behind its buffer was accepted");
    require(createGlBlob(*renderer, contextId, paddedResource, paddedBlobId,
                unalignedBytes) == 0,
        "the buffer could not be taken at its own size after a refusal");
    require(exportedBytes(*renderer, paddedResource) >= unalignedBytes,
        "the pages behind a blob are shorter than the blob");
    renderer->unrefResource(paddedResource);
}

[[noreturn]] void runMode(std::string_view mode)
{
    try {
        if (mode == "venus-unlinked-reexport")
            refuseOversizedVenusBlob(true);
        else if (mode == "venus-posix-shm-reexport")
            refuseOversizedVenusBlob(false);
        else if (mode == "venus-unlinked-use")
            useRefusedVenusBlob(true);
        else if (mode == "venus-posix-shm-use")
            useRefusedVenusBlob(false);
        else if (mode == "venus-unlinked-padding")
            boundVenusBlobByItsPages(true);
        else if (mode == "venus-posix-shm-padding")
            boundVenusBlobByItsPages(false);
        else if (mode == "venus-retention")
            refuseOversizedVenusBlobsWithoutRetention();
        else if (mode == "gl-coherent-buffer")
            refuseOversizedGlBlob();
        else
            uml::testing::fail("unknown mode");
    } catch (const std::exception& error) {
        std::exit(skipWithoutRenderer(error));
    }
    std::fputs("ok\n", stderr);
    std::fflush(stderr);
    std::_Exit(0);
}

// 0 when the child passed, 77 when its renderer is unavailable here, and 1
// when it failed, after the child's own message and how it ended.
[[nodiscard]] int runChild(const char* executable, const char* mode)
{
    std::fprintf(stderr, "=== %s ===\n", mode);
    std::fflush(stderr);
    const pid_t child = launchChild(executable, mode);
    const int status = waitForChild(child, std::chrono::seconds {120});
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
        return 77;
    if (WIFSIGNALED(status)) {
        std::fprintf(stderr, "%s: the host process was ended by signal %d (%s)\n",
            mode, WTERMSIG(status), ::strsignal(WTERMSIG(status)));
    } else {
        std::fprintf(stderr, "%s: exited with %d\n", mode,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2)
        runMode(argv[1]);
    std::vector<const char*> skipped;
    int failed = 0;
    constexpr const char* modes[] = {
        "venus-unlinked-reexport", "venus-posix-shm-reexport",
        "venus-unlinked-use", "venus-posix-shm-use",
        "venus-unlinked-padding", "venus-posix-shm-padding",
        "venus-retention"};
    for (const char* mode : modes) {
        const int result = runChild(argv[0], mode);
        if (result == 77)
            skipped.push_back(mode);
        failed += result == 1 ? 1 : 0;
    }
    // A failure wins; a case that did not run must not read as a pass.
    require(failed == 0, "a case failed");
    if (!skipped.empty()) {
        std::fputs("skipped, the renderer is unavailable here:", stderr);
        for (const char* mode : skipped)
            std::fprintf(stderr, " %s", mode);
        std::fputc('\n', stderr);
        return 77;
    }
    return 0;
}
