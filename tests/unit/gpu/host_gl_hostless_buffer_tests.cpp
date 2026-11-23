// SPDX-License-Identifier: GPL-2.0-or-later

// A gralloc BLOB buffer, such as the camera framework's JPEG output: a classic
// 2D resource of format R8, one row as wide as the buffer is long, whose bind
// flags name no GPU unit (shared, linear and the allocator's software, camera
// and codec flags). The guest writes it through its own mapping of the backing
// pages and brackets the access with transfers; the host never reads it, so
// the renderer keeps no storage for it and a texture limit of the GL driver
// does not apply.
//
// The contract under test:
//  - such a buffer is created at any size up to the renderer's bound, also
//    one a GL texture could not be (18,354,160 bytes is the largest JPEG of
//    a 12 megapixel camera) and one it could (100 and 16,385 bytes);
//  - the renderer holds no storage for it, and the bytes the guest wrote into
//    the backing are the bytes it reads back after a transfer in either
//    direction, whether the transfer is the virtio-gpu request or a
//    TRANSFER3D in a command buffer that names the padded row pitch the
//    allocator gives a buffer whose size is not a multiple of 16;
//  - a transfer whose pages are not the backing, which only a caller of the
//    host API can ask for, leaves both them and the backing as they were;
//  - a transfer that names bytes outside the buffer is refused;
//  - no other command can name it: a view, a surface, a vertex or index
//    buffer, a copy and an inline write fail the command cleanly, and the
//    presenter gets no texture;
//  - only the classic creation makes one: a blob a context creates, a blob
//    that is typed afterwards and an image import keep every check, and a
//    texture the host cannot make is refused for those exactly as it was
//    before the renderer knew this buffer (this test passes the checks of
//    those paths on the library without the change);
//  - a size the renderer does not accept is refused at creation, and every
//    other resource keeps its checks: the same size with a bind flag a GPU
//    unit uses is refused, and so is a buffer taller than one row;
//  - no more than 16,384 such buffers are alive at once: a context that goes
//    does not free the ones it had, and a buffer that is released does.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <sys/uio.h>
#include <unistd.h>

extern "C" {
#include <virgl_hw.h>
#include <virglrenderer.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;

constexpr std::uint32_t pipeTexture2dTarget = 2;
constexpr std::uint32_t virglBindShared = 1U << 20U;
constexpr std::uint32_t virglBindLinear = 1U << 22U;
constexpr std::uint32_t virglBindCameraWrite = 1U << 24U;
constexpr std::uint32_t virglBindSwReadOften = 1U << 28U;
constexpr std::uint32_t virglBindSwWriteOften = 1U << 30U;
// What minigbm names for a camera's JPEG buffer that software reads.
constexpr std::uint32_t blobBind = virglBindShared | virglBindLinear |
    virglBindCameraWrite | virglBindSwReadOften | virglBindSwWriteOften;
constexpr std::uint32_t commandInlineWrite = 9;
constexpr std::uint32_t commandTransfer3d = 43;
constexpr std::uint32_t commandPipeResourceCreate = 48;
constexpr std::uint32_t commandPipeResourceSetType = 49;
constexpr std::uint32_t blobMemoryHost3d = 2;
constexpr std::uint32_t transferToHost = 1;
constexpr std::uint32_t transferFromHost = 2;
// The most such buffers the renderer keeps alive at once.
constexpr std::uint32_t hostlessLimit = 16384;
constexpr std::uint32_t commandSetIndexBuffer = 11;
constexpr std::uint32_t commandResourceCopyRegion = 17;

unsigned failures = 0;

void check(bool ok, const char* what, const char* message)
{
    if (ok)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n", what, message);
}

[[nodiscard]] int create(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t bind, std::uint32_t width, std::uint32_t height)
{
    return renderer.createResource3d(VirglRenderer::Resource3d {
        .resourceId = id,
        .target = pipeTexture2dTarget,
        .format = virglFormatR8Unorm,
        .bind = bind,
        .width = width,
        .height = height,
        .depth = 1,
        .arraySize = 1,
    });
}

[[nodiscard]] int transfer(VirglRenderer& renderer, bool toHost,
    std::uint32_t context, std::uint32_t id, std::uint32_t x,
    std::uint32_t width)
{
    const VirglRenderer::Box box {
        .x = x, .y = 0, .z = 0, .width = width, .height = 1, .depth = 1};
    return renderer.transferResource(toHost, id, context, 0, 0, 0, box, x);
}

// What the renderer logs to stderr while `action` runs. A command that names a
// resource it cannot use reports a context error and, for some commands, still
// returns success to its caller; the log line names the resource.
std::string logOf(const std::function<void()>& action)
{
    std::fflush(stderr);
    std::FILE* file = std::tmpfile();
    const int saved = ::dup(STDERR_FILENO);
    require(file != nullptr && saved >= 0, "the log could not be captured");
    ::dup2(::fileno(file), STDERR_FILENO);
    action();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    std::string text;
    std::rewind(file);
    std::array<char, 512> chunk {};
    while (const std::size_t count =
               std::fread(chunk.data(), 1, chunk.size(), file))
        text.append(chunk.data(), count);
    std::fclose(file);
    std::fputs(text.c_str(), stderr);
    return text;
}

void pattern(std::vector<std::uint8_t>& bytes)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(index * 131U + 7U);
}

// A buffer of `size` bytes, written and read back through the transfers.
void roundTrip(VirglRenderer& renderer, std::uint32_t id, std::uint32_t size)
{
    char name[64];
    std::snprintf(name, sizeof(name), "buffer of %u bytes", size);

    check(create(renderer, id, blobBind, size, 1) == 0, name,
        "the buffer was not created");
    check(renderer.resourceStorageBytes(id) == 0, name,
        "the renderer holds storage for it");

    std::vector<std::uint8_t> backing(size);
    pattern(backing);
    const std::vector<std::uint8_t> written = backing;
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, name,
        "its backing was not attached");
    check(renderer.attachResource(contextId, id) == 0, name,
        "it was not attached to the context");

    check(transfer(renderer, true, contextId, id, 0, size) == 0, name,
        "the transfer to the host failed");
    check(backing == written, name, "the transfer to the host changed it");
    check(transfer(renderer, false, contextId, id, 0, size) == 0, name,
        "the transfer from the host failed");
    check(backing == written, name,
        "the transfer from the host did not leave the bytes written");

    // The guest rewrites a part of it, as the camera does while it fills the
    // buffer, and the transfers neither undo nor repeat the write.
    const std::uint32_t part = std::min<std::uint32_t>(size / 2U, 4096U);
    std::fill_n(backing.begin() + size / 4U, part, std::uint8_t {0xa5});
    const std::vector<std::uint8_t> rewritten = backing;
    check(transfer(renderer, true, contextId, id, size / 4U, part) == 0, name,
        "the transfer of a part to the host failed");
    check(transfer(renderer, false, contextId, id, 0, size) == 0, name,
        "the transfer of the whole from the host failed");
    check(backing == rewritten, name,
        "the transfers did not leave the rewritten bytes");

    VirglRenderer::NativeTexture texture;
    check(renderer.resourceMetalTexture(id, texture) != 0, name,
        "a presenter was given a texture for it");

    (void)renderer.detachResource(contextId, id);
    renderer.detachBacking(id);
    renderer.unrefResource(id);
}

// Names the buffer in commands that need a texture or a host buffer. Each one
// ends the context it is sent to, so each goes to a context of its own.
void commandsRefused(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t firstContext)
{
    constexpr std::uint32_t size = 100000;
    check(create(renderer, id, blobBind, size, 1) == 0, "commands",
        "the buffer was not created");
    std::vector<std::uint8_t> backing(size);
    pattern(backing);
    const std::vector<std::uint8_t> written = backing;
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, "commands",
        "its backing was not attached");

    // An ordinary texture to copy to and from.
    const std::uint32_t other = id + 1000;
    check(create(renderer, other, virglBindSamplerView | virglBindRenderTarget,
              16, 16) == 0,
        "commands", "the texture to copy was not created");

    struct Attempt {
        const char* name;
        std::uint32_t command;
        std::uint32_t object;
        std::vector<std::uint32_t> payload;
    };
    const std::vector<Attempt> attempts {
        {"sampler view", commandCreateObject, objectSamplerView,
            {firstTransientHandle, id, virglFormatR8Unorm | (2U << 24U), 0, 0,
                0U | (1U << 3U) | (2U << 6U) | (3U << 9U)}},
        {"surface", commandCreateObject, objectSurface,
            {firstTransientHandle, id, virglFormatR8Unorm, 0, 0}},
        {"vertex buffer", commandSetVertexBuffers, 0, {4, 0, id}},
        {"index buffer", commandSetIndexBuffer, 0, {id, 1, 0}},
        {"copy from it", commandResourceCopyRegion, 0,
            {other, 0, 0, 0, 0, id, 0, 0, 0, 0, 1, 1, 1}},
        {"copy into it", commandResourceCopyRegion, 0,
            {id, 0, 0, 0, 0, other, 0, 0, 0, 0, 1, 1, 1}},
        {"inline write", commandInlineWrite, 0,
            {id, 0, 0, 0, 0, 0, 0, 0, 4, 1, 1, 0x01020304U}},
    };

    std::uint32_t context = firstContext;
    for (const Attempt& attempt : attempts) {
        check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
            attempt.name, "the context was not created");
        check(renderer.attachResource(context, id) == 0, attempt.name,
            "the buffer was not attached to the context");
        check(renderer.attachResource(context, other) == 0, attempt.name,
            "the texture was not attached to the context");
        std::vector<std::uint32_t> words;
        words.push_back(
            (static_cast<std::uint32_t>(attempt.payload.size()) << 16U) |
            (attempt.object << 8U) | attempt.command);
        words.insert(words.end(), attempt.payload.begin(),
            attempt.payload.end());
        const std::string log = logOf([&] {
            (void)renderer.submit(
                context,
                {reinterpret_cast<const std::uint8_t*>(words.data()),
                    words.size() * sizeof(std::uint32_t)});
        });
        check(log.find("context error reported " + std::to_string(context)) !=
                std::string::npos,
            attempt.name, "a command naming the buffer was accepted");
        check(log.find("Illegal resource " + std::to_string(id)) !=
                std::string::npos,
            attempt.name, "the error did not name the buffer");
        renderer.destroyContext(context);
        ++context;
    }
    check(backing == written, "commands", "a refused command changed it");

    renderer.detachBacking(id);
    renderer.unrefResource(id);
    renderer.unrefResource(other);
}

// A transfer outside the buffer is refused, and ends the context it is sent to.
void transferRefused(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t context)
{
    constexpr std::uint32_t size = 4096;
    check(create(renderer, id, blobBind, size, 1) == 0, "transfer",
        "the buffer was not created");
    std::vector<std::uint8_t> backing(size);
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, "transfer",
        "its backing was not attached");
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "transfer", "the context was not created");
    check(renderer.attachResource(context, id) == 0, "transfer",
        "the buffer was not attached to the context");
    check(transfer(renderer, true, context, id, size - 1, 2) != 0, "transfer",
        "a box past the end of the buffer was accepted");
    renderer.destroyContext(context);
    renderer.detachBacking(id);
    renderer.unrefResource(id);
}

// A buffer without backing: nothing was written, and a transfer is refused as
// for any resource without backing.
void transferWithoutBacking(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t context)
{
    constexpr std::uint32_t size = 4096;
    check(create(renderer, id, blobBind, size, 1) == 0, "no backing",
        "the buffer was not created");
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "no backing", "the context was not created");
    check(renderer.attachResource(context, id) == 0, "no backing",
        "the buffer was not attached to the context");
    check(transfer(renderer, true, context, id, 0, size) != 0, "no backing",
        "a transfer without backing was accepted");
    renderer.destroyContext(context);
    renderer.unrefResource(id);
}

void sizesRefused(VirglRenderer& renderer, std::uint32_t id)
{
    for (const std::uint32_t width :
        {0U, (1U << 30U) + 1U, 0x7fffffffU, 0xffffffffU}) {
        char name[64];
        std::snprintf(name, sizeof(name), "width %u", width);
        const int result = create(renderer, id, blobBind, width, 1);
        check(result != 0, name, "the buffer was created");
        if (result == 0)
            renderer.unrefResource(id);
    }

    // The checks of every other resource stay: the same width with a bind flag
    // a GPU unit uses is a texture the host cannot make, and a buffer of more
    // than one row is a texture too.
    for (const std::uint32_t bind : {blobBind | virglBindSamplerView,
             blobBind | virglBindRenderTarget, virglBindSamplerView}) {
        char name[64];
        std::snprintf(name, sizeof(name), "bind 0x%x", bind);
        const int result = create(renderer, id, bind, 18354160, 1);
        check(result != 0, name, "a texture past the limit was created");
        if (result == 0)
            renderer.unrefResource(id);
    }
    const int tall = create(renderer, id, blobBind, 18354160, 2);
    check(tall != 0, "two rows", "a texture past the limit was created");
    if (tall == 0)
        renderer.unrefResource(id);

    // A texture the GPU samples keeps its storage.
    check(create(renderer, id, virglBindSamplerView | virglBindShared, 64,
              64) == 0,
        "sampled texture", "it was not created");
    check(renderer.resourceStorageBytes(id) != 0, "sampled texture",
        "it has no storage");
    renderer.unrefResource(id);
}

// A transfer in a command buffer is a transfer: it names the row pitch, which
// the allocator pads to 16 bytes for a buffer whose size is not a multiple of
// it, and succeeds like the virtio-gpu request and moves nothing.
void paddedTransfer3d(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t context)
{
    constexpr std::uint32_t size = 100001;
    constexpr std::uint32_t pitch = 100016;
    check(create(renderer, id, blobBind, size, 1) == 0, "TRANSFER3D",
        "the buffer was not created");
    std::vector<std::uint8_t> backing(pitch);
    pattern(backing);
    const std::vector<std::uint8_t> written = backing;
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, "TRANSFER3D",
        "its backing was not attached");
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "TRANSFER3D", "the context was not created");
    check(renderer.attachResource(context, id) == 0, "TRANSFER3D",
        "the buffer was not attached to the context");

    struct Box {
        const char* name;
        std::uint32_t direction;
        std::uint32_t x;
        std::uint32_t width;
    };
    for (const Box& box : {Box {"whole to the host", transferToHost, 0, size},
             Box {"whole from the host", transferFromHost, 0, size},
             Box {"part to the host", transferToHost, 1000, 5000},
             Box {"part from the host", transferFromHost, 99000, 1001}}) {
        Commands commands;
        commands.add(commandTransfer3d, 0,
            {id, 0, 0, pitch, 0, box.x, 0, 0, box.width, 1, 1, box.x,
                box.direction});
        check(commands.submit(renderer, context) == 0, box.name,
            "the transfer in the command buffer failed");
        check(backing == written, box.name,
            "the transfer in the command buffer changed the bytes");
    }

    // A box past the end of the buffer is refused, as for any transfer.
    Commands past;
    past.add(commandTransfer3d, 0,
        {id, 0, 0, pitch, 0, size - 1, 0, 0, 2, 1, 1, size - 1,
            transferFromHost});
    check(past.submit(renderer, context) != 0, "TRANSFER3D",
        "a box past the end of the buffer was accepted");

    renderer.destroyContext(context);
    renderer.detachBacking(id);
    renderer.unrefResource(id);
}

// The renderer's own readback and the host API's explicit-iov transfers name
// pages that are not the backing. They are accepted and leave those pages and
// the backing as they were: the backing is all the resource holds.
void explicitPages(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t context)
{
    constexpr std::uint32_t size = 100;
    check(create(renderer, id, blobBind, size, 1) == 0, "explicit pages",
        "the buffer was not created");
    std::vector<std::uint8_t> backing(size);
    pattern(backing);
    const std::vector<std::uint8_t> written = backing;
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, "explicit pages",
        "its backing was not attached");
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "explicit pages", "the context was not created");
    check(renderer.attachResource(context, id) == 0, "explicit pages",
        "the buffer was not attached to the context");

    virgl_box box {.x = 0, .y = 0, .z = 0, .w = size, .h = 1, .d = 1};
    std::vector<std::uint8_t> pages(size, 0xcc);
    iovec other {.iov_base = pages.data(), .iov_len = pages.size()};
    check(virgl_renderer_transfer_read_iov(
              id, context, 0, 0, 0, &box, 0, &other, 1) == 0,
        "explicit pages", "a read into other pages failed");
    check(std::all_of(pages.begin(), pages.end(),
              [](std::uint8_t byte) { return byte == 0xcc; }),
        "explicit pages", "a read wrote the other pages");
    check(backing == written, "explicit pages", "a read changed the backing");

    std::fill(pages.begin(), pages.end(), std::uint8_t {0xee});
    check(virgl_renderer_transfer_write_iov(
              id, context, 0, 0, 0, &box, 0, &other, 1) == 0,
        "explicit pages", "a write from other pages failed");
    check(backing == written, "explicit pages",
        "a write from other pages reached the backing");

    // The renderer's own readback, which names the caller's pages and no
    // context.
    std::vector<std::uint8_t> readback(size * 4U, 0xcc);
    check(renderer.readResourceRectangle(id, readback, 0, 0, size, 1) == 0,
        "explicit pages", "the readback failed");
    check(std::all_of(readback.begin(), readback.end(),
              [](std::uint8_t byte) { return byte == 0xcc; }),
        "explicit pages", "the readback wrote the caller's pages");
    check(backing == written, "explicit pages",
        "the readback changed the backing");

    renderer.destroyContext(context);
    renderer.detachBacking(id);
    renderer.unrefResource(id);
}

// What the renderer logged while `action` ran names why it refused.
[[nodiscard]] bool refusedAsTexture(const std::string& log, std::uint32_t width)
{
    return log.find("2D Texture size components (" + std::to_string(width) +
               ", 1) exceeds supported value") != std::string::npos;
}

// Only the classic creation makes a buffer without storage. The other
// constructors of a resource keep every check they had: with the shape of
// such a buffer but one bind flag, a width a GL texture cannot have is
// refused and one it can have gets its texture, exactly as on the library
// without the change.
void otherPathsKeepTheirChecks(VirglRenderer& renderer, std::uint32_t id)
{
    constexpr std::uint32_t tooWide = 16385;
    constexpr std::uint32_t narrow = 100;

    // A PIPE_RESOURCE_CREATE in a command buffer, and the blob that names it.
    for (const std::uint32_t width : {tooWide, narrow}) {
        const std::uint32_t blobId = 900 + width % 7U;
        char name[64];
        std::snprintf(name, sizeof(name), "pipe resource of %u", width);
        Commands commands;
        commands.add(commandPipeResourceCreate, 0,
            {pipeTexture2d, virglFormatR8Unorm, virglBindShared, width, 1, 1,
                1, 0, 0, 0, blobId});
        int submitted = 0;
        const std::string log = logOf([&] {
            submitted = commands.submit(renderer, contextId);
        });
        if (width == tooWide) {
            check(submitted != 0 && refusedAsTexture(log, width), name,
                "it was not refused as a texture past the limit");
            continue;
        }
        check(submitted == 0, name, "it was not created");
        const int blob = renderer.createResourceBlob(
            VirglRenderer::ResourceBlob {.resourceId = id,
                .contextId = contextId,
                .memory = blobMemoryHost3d,
                .flags = 0,
                .blobId = blobId,
                .size = width});
        check(blob == 0, name, "the blob that names it was not created");
        check(renderer.resourceStorageBytes(id) != 0, name,
            "it holds no texture");
        renderer.unrefResource(id);
    }

    // The type of an untyped blob: a shared-memory segment another context
    // exported, which the guest then describes as an R8 image.
    const std::uint32_t segmentBytes = 64 * 1024;
    char segmentName[] = "/tmp/uml-hostless.XXXXXX";
    const int segment = ::mkstemp(segmentName);
    require(segment >= 0 && ::unlink(segmentName) == 0 &&
            ::ftruncate(segment, segmentBytes) == 0,
        "the segment could not be made");
    for (const std::uint32_t width : {tooWide, narrow}) {
        char name[64];
        std::snprintf(name, sizeof(name), "typed blob of %u", width);
        const int descriptor = ::dup(segment);
        require(descriptor >= 0, "the segment could not be duplicated");
        virgl_renderer_resource_import_blob_args arguments {};
        arguments.res_handle = id;
        arguments.blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D;
        arguments.fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
        arguments.fd = descriptor;
        arguments.size = segmentBytes;
        require(virgl_renderer_resource_import_blob(&arguments) == 0,
            "the segment could not be imported as a blob");
        require(renderer.attachResource(contextId, id) == 0,
            "the blob could not be attached to the guest context");
        Commands commands;
        commands.add(commandPipeResourceSetType, 0,
            {id, virglFormatR8Unorm, virglBindShared, width, 1, 0, 0, 0,
                (width + 15U) & ~15U, 0});
        int submitted = 0;
        const std::string log = logOf([&] {
            submitted = commands.submit(renderer, contextId);
        });
        if (width == tooWide) {
            check(submitted != 0 && refusedAsTexture(log, width), name,
                "it was not refused as a texture past the limit");
        } else {
            check(submitted == 0, name, "it was not typed");
            check(renderer.resourceStorageBytes(id) != 0, name,
                "it holds no texture");
        }
        (void)renderer.detachResource(contextId, id);
        renderer.unrefResource(id);
    }
    ::close(segment);

    // An imported image, here with the host API's call and no image.
    for (const std::uint32_t width : {tooWide, narrow}) {
        char name[64];
        std::snprintf(name, sizeof(name), "imported image of %u", width);
        virgl_renderer_resource_create_args arguments {
            .handle = id,
            .target = pipeTexture2dTarget,
            .format = virglFormatR8Unorm,
            .bind = virglBindShared,
            .width = width,
            .height = 1,
            .depth = 1,
            .array_size = 1,
        };
        int result = 0;
        const std::string log = logOf([&] {
            result = virgl_renderer_resource_import_eglimage(
                &arguments, nullptr);
        });
        if (width == tooWide) {
            check(result != 0 && refusedAsTexture(log, width), name,
                "it was not refused as a texture past the limit");
            if (result == 0)
                renderer.unrefResource(id);
            continue;
        }
        check(result == 0, name, "it was not created");
        check(renderer.resourceStorageBytes(id) != 0, name,
            "it holds no texture");
        renderer.unrefResource(id);
    }
}

// No more than hostlessLimit such buffers are alive at once, whoever holds
// them: a context that goes away does not release the ones it had, and
// releasing one makes room for one.
void countBounded(VirglRenderer& renderer, std::uint32_t firstId,
    std::uint32_t context)
{
    std::uint32_t created = 0;
    for (; created <= hostlessLimit + 1U; ++created) {
        std::string log;
        int result = 0;
        log = logOf([&] {
            result = create(renderer, firstId + created, blobBind, 4096, 1);
        });
        if (result != 0) {
            check(log.find("the most accepted") != std::string::npos,
                "count", "the refusal did not say why");
            break;
        }
    }
    check(created == hostlessLimit, "count",
        "the number of buffers made before a refusal is not the limit");

    // Some of them are attached to a context that then goes away.
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "count", "the context was not created");
    for (std::uint32_t index = 0; index < 4; ++index)
        check(renderer.attachResource(context, firstId + index) == 0, "count",
            "a buffer was not attached to the context");
    renderer.destroyContext(context);
    check(create(renderer, firstId + created, blobBind, 4096, 1) != 0,
        "count", "a buffer was made after the context went");

    // Releasing one makes room for exactly one.
    renderer.unrefResource(firstId);
    check(create(renderer, firstId + created, blobBind, 4096, 1) == 0, "count",
        "a buffer was refused after one was released");
    check(create(renderer, firstId + created + 1U, blobBind, 4096, 1) != 0,
        "count", "two buffers were made for one released");

    for (std::uint32_t index = 1; index <= created; ++index)
        renderer.unrefResource(firstId + index);
    check(create(renderer, firstId, blobBind, 4096, 1) == 0, "count",
        "a buffer was refused with none alive");
    renderer.unrefResource(firstId);
}

} // namespace

int main()
{
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(
                contextId, virgl2CapsetId, "hostless buffer") == 0,
        "the guest context could not be created");

    std::uint32_t id = 10;
    roundTrip(*renderer, id++, 18354160);
    roundTrip(*renderer, id++, 16385);
    roundTrip(*renderer, id++, 100);
    roundTrip(*renderer, id++, 1U << 20U);

    commandsRefused(*renderer, id++, 20);
    transferRefused(*renderer, id++, 40);
    transferWithoutBacking(*renderer, id++, 41);
    sizesRefused(*renderer, id++);
    paddedTransfer3d(*renderer, id++, 42);
    explicitPages(*renderer, id++, 43);
    otherPathsKeepTheirChecks(*renderer, id++);
    countBounded(*renderer, 1000, 44);

    if (failures != 0)
        return 1;
    std::fputs("Host-less buffer: ok\n", stderr);
    return 0;
}
