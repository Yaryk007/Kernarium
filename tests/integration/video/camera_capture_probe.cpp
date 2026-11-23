// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe of the Virtio camera's V4L2 nodes. The emulator runs with the
// synthetic camera's tablet profile (a back camera with a torch and Bayer
// stills, and a front camera), either live (`synthetic`) or started blocked
// (`blocked`); the mode is the probe's first argument. The probe builds in the
// guest with the guest's own C++ compiler and kernel headers.
//
// Both modes check that the camera nodes sit at /dev/video10 and above, that
// each camera has a capture node and a metadata node, what QUERYCAP, the
// formats, the frame sizes and intervals and the controls say (an integer
// control is clamped to its range, a menu entry the camera skips is refused,
// and a frame interval beyond the camera's is brought to one it has), that the
// camera's description control matches its node, and that 40 files can be
// open on a node with no host stream behind them.
//
// `synthetic` then streams NV12 by MMAP, by two files of one node together,
// and by DMABUF imported from another file's exported buffers, and decodes
// the synthetic chart's marker in each frame (camera tag and a counter that
// keeps rising). It reads the metadata node alongside a video stream, and two
// metadata files at once, and matches the records to the frames by sequence,
// reads the volatile focus status, writes the zoom and the exposure controls
// and sees them in the next records, takes a JPEG and a Bayer
// still (alone and as one group, which shares the exposure's sequence) with the
// still's record on the metadata node, and writes the torch with no stream.
//
// `blocked` checks that V4L2_CID_PRIVACY reads 1, that frames are black and
// not failed, that the metadata says so, and that a still completes failed.
//
// `hold` streams video and metadata with a control written, printing a line a
// second, until /tmp/camera-stop exists; run_camera_suspend.py suspends the
// guest meanwhile. The frames must keep coming, with no failed buffer, the
// sequences rising and the control still in force in the records, and every
// buffer of both queues must come back after each suspend (a line counts the
// distinct buffers returned since the last gap in the frames).
//
// The exit status is the number of failed checks.

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef V4L2_PIX_FMT_SRGGB16
#define V4L2_PIX_FMT_SRGGB16 v4l2_fourcc('R', 'G', '1', '6')
#endif
#ifndef V4L2_PIX_FMT_SGRBG16
#define V4L2_PIX_FMT_SGRBG16 v4l2_fourcc('G', 'R', '1', '6')
#endif
#ifndef V4L2_PIX_FMT_SGBRG16
#define V4L2_PIX_FMT_SGBRG16 v4l2_fourcc('G', 'B', '1', '6')
#endif
#ifndef V4L2_PIX_FMT_SBGGR16
#define V4L2_PIX_FMT_SBGGR16 v4l2_fourcc('B', 'Y', 'R', '2')
#endif

namespace {

// ---------------------------------------------------------------------------
// The device's records (kernel/common/video/virtio_video.h)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kCidBase = 0x00981c00u;
constexpr std::uint32_t kCidExposureNs = kCidBase + 0;
constexpr std::uint32_t kCidIso = kCidBase + 1;
constexpr std::uint32_t kCidZoomQ16 = kCidBase + 5;
constexpr std::uint32_t kCidDescription = kCidBase + 16;

constexpr std::uint32_t kFacingBack = 1;
constexpr std::uint32_t kFacingFront = 2;
constexpr std::uint32_t kCameraTorch = 1u << 2;
constexpr std::uint32_t kCameraManualExposure = 1u << 4;

constexpr std::uint32_t kFrameBlack = 1u << 0;
constexpr std::uint32_t kFrameBlocked = 1u << 2;
constexpr std::uint32_t kFrameStill = 1u << 4;

constexpr std::uint32_t kMetaFormat = v4l2_fourcc('V', 'C', 'A', 'M');
constexpr int kExpectedCameras = 2;
constexpr int kFirstNode = 10;

struct CameraDesc {
    std::uint32_t index, facing, lens, flags;
    std::uint32_t conflict_mask, concurrent_mask, constituent_mask, sensor_orientation;
    std::uint32_t pixel_array_width, pixel_array_height;
    std::uint32_t sensor_width_um, sensor_height_um, focal_length_um, aperture_x100;
    std::uint32_t min_focus_distance_mm, iso_min, iso_max;
    std::uint8_t reserved0[4];
    std::uint64_t exposure_min_ns, exposure_max_ns;
    std::uint32_t zoom_min_q16, zoom_max_q16, num_switch_overs, switch_over_q16[4];
    std::uint32_t max_wb_gain_q16, intrinsics_q16[4];
    std::uint32_t bayer_order, black_level, white_level, raw_width, raw_height;
    std::uint32_t reference_illuminant1, reference_illuminant2;
    std::uint32_t color_matrix1_q16[9], color_matrix2_q16[9];
    std::uint32_t forward_matrix1_q16[9], forward_matrix2_q16[9];
    std::uint8_t reserved[20];
};
static_assert(sizeof(CameraDesc) == 328, "the camera description is 328 bytes");

struct CameraMeta {
    std::uint64_t sequence, exposure_ns, frame_duration_ns;
    std::uint32_t iso, flags, ae_state, af_state, awb_state, lens_position_q16;
    std::uint32_t wb_gain_q16[3], color_temperature_k, tint_q16, zoom_ratio_q16, active_camera;
    std::uint32_t aperture_x100, focal_length_um, flash_state;
    std::uint32_t neutral_point_q16[3], noise_profile_f32[8];
    std::uint32_t jpeg_gap_offset, jpeg_gap_length;
    std::uint8_t reserved[28];
};
static_assert(sizeof(CameraMeta) == 168, "the metadata record is 168 bytes");

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

int failures = 0;
bool blocked_mode = false;

__attribute__((format(printf, 3, 4))) void fail(int line, const char* expression, const char* format, ...)
{
    ++failures;
    std::printf("FAIL line %d: %s: ", line, expression);
    va_list arguments;
    va_start(arguments, format);
    std::vprintf(format, arguments);
    va_end(arguments);
    std::printf("\n");
    std::fflush(stdout);
}

#define CHECK(condition, ...)                                  \
    do {                                                       \
        if (!(condition)) fail(__LINE__, #condition, __VA_ARGS__); \
    } while (0)

void step(const char* name)
{
    std::printf("== %s\n", name);
    std::fflush(stdout);
}

int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

std::uint64_t monotonic_ns()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return std::uint64_t(now.tv_sec) * 1000000000ull + std::uint64_t(now.tv_nsec);
}

// The clock that keeps counting while the guest is suspended.
std::uint64_t boottime_ns()
{
    timespec now {};
    clock_gettime(CLOCK_BOOTTIME, &now);
    return std::uint64_t(now.tv_sec) * 1000000000ull + std::uint64_t(now.tv_nsec);
}

// ---------------------------------------------------------------------------
// The chart (darwin/platform/synthetic_camera): a row of 64 cells across the
// middle of the picture carries the camera's tag, a frame counter and a check
// word; a centred crop and a scale of the picture keep it.
// ---------------------------------------------------------------------------

struct Marker {
    bool valid = false;
    std::uint16_t tag = 0;
    std::uint32_t counter = 0;
};

Marker decode_marker(const std::uint8_t* luma, std::size_t stride, std::uint32_t width, std::uint32_t height)
{
    constexpr std::uint32_t cells = 64;
    constexpr std::uint32_t cell_height = 8;
    Marker marker;
    if (height < cell_height || width < 2 * cells) return marker;
    // The marker's rows begin at the first even row of the frame's middle; its cells share the width.
    const std::size_t row = (((height - cell_height) / 2) & ~1u) + cell_height / 2;
    std::uint64_t bits = 0;
    for (std::uint32_t index = 0; index < cells; ++index)
        bits = (bits << 1) | (luma[row * stride + std::size_t(2 * index + 1) * width / (2 * cells)] >= 128 ? 1u : 0u);
    const std::uint16_t check = std::uint16_t(bits);
    const std::uint64_t first48 = bits >> 16;
    marker.tag = std::uint16_t(first48 >> 32);
    marker.counter = std::uint32_t(first48);
    marker.valid = check == std::uint16_t((first48 >> 32) ^ (first48 >> 16) ^ first48);
    return marker;
}

bool nv12_black(const std::uint8_t* data, std::size_t stride, std::uint32_t width, std::uint32_t height)
{
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            if (data[std::size_t(y) * stride + x] != 0) return false;
    const std::uint8_t* chroma = data + stride * height;
    for (std::uint32_t y = 0; y < height / 2; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            if (chroma[std::size_t(y) * stride + x] != 128) return false;
    return true;
}

// The chart's patches and edges make the luma span a wide range.
bool nv12_has_content(const std::uint8_t* data, std::size_t stride, std::uint32_t width, std::uint32_t height)
{
    int low = 255, high = 0;
    for (std::uint32_t y = 16; y < height; y += 7)
        for (std::uint32_t x = 0; x < width; x += 5) {
            const int value = data[std::size_t(y) * stride + x];
            low = std::min(low, value);
            high = std::max(high, value);
        }
    return high - low >= 64;
}

// ---------------------------------------------------------------------------
// One open file of a V4L2 node
// ---------------------------------------------------------------------------

struct Dequeued {
    unsigned index = 0;
    std::uint32_t sequence = 0;
    std::uint32_t flags = 0;
    std::uint32_t bytesused = 0;
    timeval timestamp {};
};

class Node {
public:
    struct Buffer {
        void* address = nullptr;
        std::size_t length = 0;
        int dma_fd = -1;
        bool owned_mapping = false;
    };

    std::string path;
    bool meta = false;
    int fd = -1;
    v4l2_memory memory = V4L2_MEMORY_MMAP;
    std::vector<Buffer> buffers;
    std::uint32_t pixelformat = 0, width = 0, height = 0, stride = 0, sizeimage = 0;

    Node(std::string node_path, bool metadata) : path(std::move(node_path)), meta(metadata) {}
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() { close_file(); }

    v4l2_buf_type type() const { return meta ? V4L2_BUF_TYPE_META_CAPTURE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; }

    bool open_file()
    {
        fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) std::printf("cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return fd >= 0;
    }

    void close_file()
    {
        release_buffers(false);
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    bool get_format()
    {
        v4l2_format format {};
        format.type = type();
        if (xioctl(fd, VIDIOC_G_FMT, &format) < 0) return false;
        if (meta) {
            pixelformat = format.fmt.meta.dataformat;
            sizeimage = format.fmt.meta.buffersize;
            return true;
        }
        const auto& pix = format.fmt.pix_mp;
        pixelformat = pix.pixelformat;
        width = pix.width;
        height = pix.height;
        stride = pix.plane_fmt[0].bytesperline;
        sizeimage = pix.plane_fmt[0].sizeimage;
        return pix.num_planes == 1;
    }

    bool set_format(std::uint32_t fourcc, std::uint32_t w, std::uint32_t h)
    {
        v4l2_format format {};
        format.type = type();
        auto& pix = format.fmt.pix_mp;
        pix.pixelformat = fourcc;
        pix.width = w;
        pix.height = h;
        pix.num_planes = 1;
        if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) return false;
        return get_format();
    }

    bool allocate(unsigned count, v4l2_memory how)
    {
        release_buffers(true);
        memory = how;
        v4l2_requestbuffers request {};
        request.count = count;
        request.type = type();
        request.memory = how;
        if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0) return false;
        buffers.assign(request.count, Buffer {});
        if (how != V4L2_MEMORY_MMAP) return request.count == count;
        for (unsigned index = 0; index < request.count; ++index) {
            v4l2_buffer buffer {};
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            prepare(buffer, planes, index);
            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) return false;
            const std::size_t length = meta ? buffer.length : planes[0].length;
            const off_t offset = meta ? off_t(buffer.m.offset) : off_t(planes[0].m.mem_offset);
            void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
            if (address == MAP_FAILED) return false;
            buffers[index].address = address;
            buffers[index].length = length;
            buffers[index].owned_mapping = true;
        }
        return request.count == count;
    }

    void release_buffers(bool free_in_driver)
    {
        for (auto& buffer : buffers) {
            if (buffer.owned_mapping && buffer.address != nullptr) munmap(buffer.address, buffer.length);
            if (buffer.dma_fd >= 0 && buffer.owned_mapping) ::close(buffer.dma_fd);
        }
        buffers.clear();
        if (free_in_driver && fd >= 0) {
            v4l2_requestbuffers request {};
            request.count = 0;
            request.type = type();
            request.memory = memory;
            xioctl(fd, VIDIOC_REQBUFS, &request);
        }
    }

    // Exports every MMAP buffer as a dma-buf.
    bool export_buffers()
    {
        for (unsigned index = 0; index < buffers.size(); ++index) {
            v4l2_exportbuffer exported {};
            exported.type = type();
            exported.index = index;
            exported.plane = 0;
            exported.flags = O_RDWR | O_CLOEXEC;
            if (xioctl(fd, VIDIOC_EXPBUF, &exported) < 0) return false;
            buffers[index].dma_fd = exported.fd;
        }
        return true;
    }

    void prepare(v4l2_buffer& buffer, v4l2_plane* planes, unsigned index) const
    {
        std::memset(&buffer, 0, sizeof(buffer));
        std::memset(planes, 0, sizeof(*planes) * VIDEO_MAX_PLANES);
        buffer.type = type();
        buffer.memory = memory;
        buffer.index = index;
        if (!meta) {
            buffer.m.planes = planes;
            buffer.length = 1;
        }
    }

    bool queue(unsigned index, std::uint64_t cookie = 0, bool last = false)
    {
        v4l2_buffer buffer {};
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        prepare(buffer, planes, index);
        buffer.timestamp.tv_sec = time_t(cookie);
        if (last) buffer.flags |= V4L2_BUF_FLAG_LAST;
        if (memory == V4L2_MEMORY_DMABUF) {
            planes[0].m.fd = buffers[index].dma_fd;
            planes[0].length = buffers[index].length;
        }
        return xioctl(fd, VIDIOC_QBUF, &buffer) == 0;
    }

    bool queue_all()
    {
        for (unsigned index = 0; index < buffers.size(); ++index)
            if (!queue(index)) {
                std::printf("QBUF %u on %s failed: %s\n", index, path.c_str(), std::strerror(errno));
                return false;
            }
        return true;
    }

    bool stream_on()
    {
        int t = int(type());
        return xioctl(fd, VIDIOC_STREAMON, &t) == 0;
    }

    bool stream_off()
    {
        int t = int(type());
        return xioctl(fd, VIDIOC_STREAMOFF, &t) == 0;
    }

    // 1: a buffer, 0: none within the time, -1: an error.
    int dequeue(Dequeued& out, int timeout_ms)
    {
        pollfd poll_fd {fd, POLLIN, 0};
        int ready;
        do {
            ready = poll(&poll_fd, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready == 0) return 0;
        if (ready < 0) return -1;
        return dequeue_now(out);
    }

    int dequeue_now(Dequeued& out)
    {
        v4l2_buffer buffer {};
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        prepare(buffer, planes, 0);
        if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) return errno == EAGAIN ? 0 : -1;
        out.index = buffer.index;
        out.sequence = buffer.sequence;
        out.flags = buffer.flags;
        out.timestamp = buffer.timestamp;
        out.bytesused = meta ? buffer.bytesused : planes[0].bytesused;
        return 1;
    }

    const std::uint8_t* data(unsigned index) const { return static_cast<const std::uint8_t*>(buffers[index].address); }
};

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

struct Camera {
    std::uint32_t index = 0;
    int capture_number = -1;
    int meta_number = -1;
    CameraDesc desc {};
    bool have_desc = false;

    std::string capture_path() const { return "/dev/video" + std::to_string(capture_number); }
    std::string meta_path() const { return "/dev/video" + std::to_string(meta_number); }
    bool back() const { return desc.facing == kFacingBack; }
};

std::string read_line(const std::string& path)
{
    std::string line;
    if (FILE* file = std::fopen(path.c_str(), "r")) {
        char text[128] {};
        if (std::fgets(text, sizeof(text), file)) line = text;
        std::fclose(file);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

std::vector<Camera> discover()
{
    std::map<std::uint32_t, Camera> cameras;
    DIR* directory = opendir("/sys/class/video4linux");
    CHECK(directory != nullptr, "cannot read /sys/class/video4linux");
    if (directory == nullptr) return {};
    while (const dirent* entry = readdir(directory)) {
        int number = -1;
        if (std::sscanf(entry->d_name, "video%d", &number) != 1) continue;
        const std::string name = read_line(std::string("/sys/class/video4linux/") + entry->d_name + "/name");
        unsigned index = 0;
        char kind[16] {};
        if (std::sscanf(name.c_str(), "virtio-camera%u-%15s", &index, kind) != 2) {
            std::printf("video%d: %s (not a camera)\n", number, name.c_str());
            continue;
        }
        std::printf("video%d: %s\n", number, name.c_str());
        CHECK(number >= kFirstNode, "camera node video%d is below video%d", number, kFirstNode);
        auto& camera = cameras[index];
        camera.index = index;
        if (std::strcmp(kind, "capture") == 0)
            camera.capture_number = number;
        else if (std::strcmp(kind, "meta") == 0)
            camera.meta_number = number;
        else
            CHECK(false, "node %s has an unknown kind", name.c_str());
    }
    closedir(directory);
    std::vector<Camera> result;
    for (auto& [index, camera] : cameras) {
        CHECK(camera.capture_number >= 0 && camera.meta_number >= 0, "camera %u lacks a capture or a metadata node", index);
        if (camera.capture_number >= 0 && camera.meta_number >= 0) result.push_back(camera);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

struct Control {
    std::uint32_t id = 0, type = 0, flags = 0;
    std::int64_t minimum = 0, maximum = 0;
    std::string name;
};

std::vector<Control> enumerate_controls(int fd)
{
    std::vector<Control> controls;
    v4l2_query_ext_ctrl query {};
    query.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    while (xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &query) == 0 && controls.size() < 512) {
        Control control;
        control.id = query.id;
        control.type = query.type;
        control.flags = query.flags;
        control.minimum = query.minimum;
        control.maximum = query.maximum;
        control.name = query.name;
        controls.push_back(control);
        query.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
    return controls;
}

const Control* find_control(const std::vector<Control>& controls, std::uint32_t id)
{
    for (const auto& control : controls)
        if (control.id == id) return &control;
    return nullptr;
}

bool set_control(int fd, std::uint32_t id, std::int64_t value, bool wide = false)
{
    v4l2_ext_control control {};
    control.id = id;
    if (wide)
        control.value64 = value;
    else
        control.value = std::int32_t(value);
    v4l2_ext_controls controls {};
    controls.which = V4L2_CTRL_ID2WHICH(id);
    controls.count = 1;
    controls.controls = &control;
    return xioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0;
}

bool get_control(int fd, std::uint32_t id, std::int64_t& value, bool wide = false)
{
    v4l2_ext_control control {};
    control.id = id;
    v4l2_ext_controls controls {};
    controls.which = V4L2_CTRL_ID2WHICH(id);
    controls.count = 1;
    controls.controls = &control;
    if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) < 0) return false;
    value = wide ? std::int64_t(control.value64) : std::int64_t(control.value);
    return true;
}

bool read_description(int fd, CameraDesc& desc, int* error = nullptr)
{
    v4l2_ext_control control {};
    control.id = kCidDescription;
    control.size = sizeof(desc);
    control.ptr = &desc;
    v4l2_ext_controls controls {};
    controls.which = V4L2_CTRL_ID2WHICH(kCidDescription);
    controls.count = 1;
    controls.controls = &control;
    const int result = xioctl(fd, VIDIOC_G_EXT_CTRLS, &controls);
    if (error) *error = result < 0 ? errno : 0;
    return result == 0;
}

// ---------------------------------------------------------------------------
// Both modes: nodes, formats, controls
// ---------------------------------------------------------------------------

void test_querycap(const Camera& camera)
{
    for (const bool meta : {false, true}) {
        Node node(meta ? camera.meta_path() : camera.capture_path(), meta);
        if (!node.open_file()) {
            CHECK(false, "open %s", node.path.c_str());
            continue;
        }
        v4l2_capability capability {};
        CHECK(xioctl(node.fd, VIDIOC_QUERYCAP, &capability) == 0, "QUERYCAP %s", node.path.c_str());
        char want[32];
        std::snprintf(want, sizeof(want), "virtio-camera:%u", camera.index);
        CHECK(std::strcmp(reinterpret_cast<const char*>(capability.bus_info), want) == 0, "bus_info of %s is '%s', not '%s'",
              node.path.c_str(), capability.bus_info, want);
        CHECK(capability.capabilities & V4L2_CAP_DEVICE_CAPS, "%s lacks device caps", node.path.c_str());
        CHECK(capability.device_caps & V4L2_CAP_STREAMING, "%s cannot stream", node.path.c_str());
        if (meta)
            CHECK(capability.device_caps & V4L2_CAP_META_CAPTURE, "%s is not a metadata capture node", node.path.c_str());
        else
            CHECK(capability.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE, "%s is not a multi-planar capture node",
                  node.path.c_str());
        CHECK(capability.driver[0] != 0 && capability.card[0] != 0, "%s has no driver or card name", node.path.c_str());
    }
}

void test_description(Camera& camera)
{
    Node node(camera.capture_path(), false);
    if (!node.open_file()) return;
    camera.have_desc = read_description(node.fd, camera.desc);
    CHECK(camera.have_desc, "the description control of camera %u: %s", camera.index, std::strerror(errno));
    if (!camera.have_desc) return;
    CHECK(camera.desc.index == camera.index, "the description says camera %u, the node camera %u", camera.desc.index,
          camera.index);
    CHECK(camera.desc.facing == kFacingBack || camera.desc.facing == kFacingFront, "facing %u", camera.desc.facing);
    CHECK(camera.desc.iso_max >= camera.desc.iso_min, "ISO range %u..%u", camera.desc.iso_min, camera.desc.iso_max);
    std::printf("camera %u: facing %u lens %u flags %#x iso %u..%u white level %u\n", camera.index, camera.desc.facing,
                camera.desc.lens, camera.desc.flags, camera.desc.iso_min, camera.desc.iso_max, camera.desc.white_level);

    // A buffer too small for the record says how large the control is.
    CameraDesc small {};
    v4l2_ext_control control {};
    control.id = kCidDescription;
    control.size = 16;
    control.ptr = &small;
    v4l2_ext_controls controls {};
    controls.which = V4L2_CTRL_ID2WHICH(kCidDescription);
    controls.count = 1;
    controls.controls = &control;
    CHECK(xioctl(node.fd, VIDIOC_G_EXT_CTRLS, &controls) < 0 && errno == ENOSPC, "a 16-byte read of the description: %s",
          std::strerror(errno));
    CHECK(control.size == sizeof(CameraDesc), "the description is %u bytes", control.size);

    // The metadata node answers with the same record.
    Node meta(camera.meta_path(), true);
    if (meta.open_file()) {
        CameraDesc other {};
        CHECK(read_description(meta.fd, other) && std::memcmp(&other, &camera.desc, sizeof(other)) == 0,
              "the metadata node's description differs");
    }
}

void test_controls(const Camera& camera)
{
    Node capture(camera.capture_path(), false);
    Node meta(camera.meta_path(), true);
    if (!capture.open_file() || !meta.open_file()) return;
    const auto controls = enumerate_controls(capture.fd);
    const auto other = enumerate_controls(meta.fd);
    std::printf("camera %u: %zu controls\n", camera.index, controls.size());
    CHECK(controls.size() > 5, "only %zu controls", controls.size());
    CHECK(controls.size() == other.size(), "the capture node lists %zu controls, the metadata node %zu", controls.size(),
          other.size());
    const Control* privacy = find_control(controls, V4L2_CID_PRIVACY);
    CHECK(privacy != nullptr && privacy->type == V4L2_CTRL_TYPE_BOOLEAN, "no privacy control");
    const Control* description = find_control(controls, kCidDescription);
    CHECK(description != nullptr && description->type == V4L2_CTRL_TYPE_U8 && (description->flags & V4L2_CTRL_FLAG_READ_ONLY),
          "the description is missing or writable");
    for (const auto& control : controls)
        CHECK(!control.name.empty(), "control %#x has no name", control.id);
    if (camera.have_desc && camera.desc.flags & kCameraTorch) {
        CHECK(find_control(controls, V4L2_CID_FLASH_LED_MODE) != nullptr, "a camera with a torch lacks the flash mode");
    }
    if (camera.have_desc && camera.desc.flags & kCameraManualExposure) {
        CHECK(find_control(controls, kCidExposureNs) != nullptr, "no exposure time in nanoseconds");
        CHECK(find_control(controls, kCidIso) != nullptr, "no ISO control");
    }
    CHECK(find_control(controls, kCidZoomQ16) != nullptr, "no zoom control");

    std::int64_t value = -1;
    CHECK(get_control(capture.fd, V4L2_CID_PRIVACY, value), "reading the privacy control: %s", std::strerror(errno));
    CHECK(get_control(meta.fd, V4L2_CID_PRIVACY, value) && value == (blocked_mode ? 1 : 0), "privacy reads %lld on the metadata node",
          static_cast<long long>(value));
    CHECK(get_control(capture.fd, V4L2_CID_PRIVACY, value) && value == (blocked_mode ? 1 : 0), "privacy reads %lld",
          static_cast<long long>(value));
    // The privacy control is the device's word: a guest cannot write it.
    CHECK(!set_control(capture.fd, V4L2_CID_PRIVACY, blocked_mode ? 0 : 1), "the guest wrote the privacy control");

    // An integer control keeps its range: a value outside it is clamped to the
    // nearest end, and what the guest reads back is what the camera holds.
    const Control* bias = find_control(controls, V4L2_CID_AUTO_EXPOSURE_BIAS);
    CHECK(bias != nullptr && bias->type == V4L2_CTRL_TYPE_INTEGER && bias->minimum < 0 && bias->maximum > 0,
          "no exposure bias with a range");
    if (bias != nullptr && bias->type == V4L2_CTRL_TYPE_INTEGER) {
        CHECK(set_control(capture.fd, V4L2_CID_AUTO_EXPOSURE_BIAS, bias->maximum + 100000), "a value over the range: %s",
              std::strerror(errno));
        CHECK(get_control(capture.fd, V4L2_CID_AUTO_EXPOSURE_BIAS, value) && value == bias->maximum,
              "a value over the range reads back %lld, not %lld", static_cast<long long>(value), static_cast<long long>(bias->maximum));
        CHECK(set_control(capture.fd, V4L2_CID_AUTO_EXPOSURE_BIAS, bias->minimum - 100000), "a value under the range: %s",
              std::strerror(errno));
        CHECK(get_control(meta.fd, V4L2_CID_AUTO_EXPOSURE_BIAS, value) && value == bias->minimum,
              "a value under the range reads back %lld on the metadata node, not %lld", static_cast<long long>(value),
              static_cast<long long>(bias->minimum));
        CHECK(set_control(capture.fd, V4L2_CID_AUTO_EXPOSURE_BIAS, 0), "the bias back to zero");
    }
}

struct SizeList {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
};

SizeList frame_sizes(int fd, std::uint32_t pixelformat)
{
    SizeList list;
    for (std::uint32_t index = 0; index < 32; ++index) {
        v4l2_frmsizeenum size {};
        size.index = index;
        size.pixel_format = pixelformat;
        if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            list.sizes.emplace_back(size.discrete.width, size.discrete.height);
        else
            list.sizes.emplace_back(size.stepwise.min_width, size.stepwise.min_height);
    }
    return list;
}

std::pair<std::uint32_t, std::uint32_t> smallest(const SizeList& list)
{
    std::pair<std::uint32_t, std::uint32_t> best {0, 0};
    for (const auto& size : list.sizes)
        if (best.first == 0 || std::uint64_t(size.first) * size.second < std::uint64_t(best.first) * best.second) best = size;
    return best;
}

void test_formats(const Camera& camera)
{
    Node node(camera.capture_path(), false);
    if (!node.open_file()) return;
    std::set<std::uint32_t> formats;
    for (std::uint32_t index = 0; index < 16; ++index) {
        v4l2_fmtdesc description {};
        description.index = index;
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(node.fd, VIDIOC_ENUM_FMT, &description) < 0) break;
        formats.insert(description.pixelformat);
        std::printf("camera %u: format %.4s '%s'\n", camera.index, reinterpret_cast<const char*>(&description.pixelformat),
                    description.description);
    }
    CHECK(formats.count(V4L2_PIX_FMT_NV12) == 1, "NV12 is not offered");
    CHECK(formats.count(V4L2_PIX_FMT_JPEG) == 1, "JPEG is not offered");
    v4l2_fmtdesc past {};
    past.index = 16;
    past.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    CHECK(xioctl(node.fd, VIDIOC_ENUM_FMT, &past) < 0, "the format list does not end");

    const SizeList video = frame_sizes(node.fd, V4L2_PIX_FMT_NV12);
    CHECK(video.sizes.size() >= 2, "%zu NV12 sizes", video.sizes.size());
    for (const auto& size : video.sizes) {
        CHECK(size.first % 2 == 0 && size.second % 2 == 0, "NV12 size %ux%u is odd", size.first, size.second);
        v4l2_frmivalenum interval {};
        interval.index = 0;
        interval.pixel_format = V4L2_PIX_FMT_NV12;
        interval.width = size.first;
        interval.height = size.second;
        if (xioctl(node.fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) {
            CHECK(false, "no frame interval for %ux%u", size.first, size.second);
            continue;
        }
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
            CHECK(interval.discrete.numerator > 0 && interval.discrete.denominator > 0, "interval of %ux%u", size.first, size.second);
        else
            CHECK(interval.stepwise.min.denominator > 0 && interval.stepwise.max.denominator > 0, "interval range of %ux%u",
                  size.first, size.second);
    }
    CHECK(!frame_sizes(node.fd, V4L2_PIX_FMT_JPEG).sizes.empty(), "no JPEG size");
    CHECK(frame_sizes(node.fd, V4L2_PIX_FMT_YUYV).sizes.empty(), "sizes of a format that is not offered");

    // A new file starts at NV12, no larger than 1280 x 720.
    CHECK(node.get_format(), "G_FMT: %s", std::strerror(errno));
    CHECK(node.pixelformat == V4L2_PIX_FMT_NV12 && node.width <= 1280 && node.height <= 720, "the default is %.4s %ux%u",
          reinterpret_cast<const char*>(&node.pixelformat), node.width, node.height);
    CHECK(node.stride >= node.width && node.sizeimage == std::uint64_t(node.stride) * (node.height + node.height / 2),
          "default layout: stride %u, image %u", node.stride, node.sizeimage);

    // TRY_FMT settles on a size the camera offers, and S_FMT keeps it.
    v4l2_format format {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.width = 641;
    format.fmt.pix_mp.height = 481;
    CHECK(xioctl(node.fd, VIDIOC_TRY_FMT, &format) == 0, "TRY_FMT: %s", std::strerror(errno));
    const std::uint32_t settled_width = format.fmt.pix_mp.width;
    const std::uint32_t settled_height = format.fmt.pix_mp.height;
    const std::pair<std::uint32_t, std::uint32_t> settled(settled_width, settled_height);
    CHECK(std::find(video.sizes.begin(), video.sizes.end(), settled) != video.sizes.end(), "TRY_FMT settled on %ux%u",
          settled.first, settled.second);
    CHECK(node.set_format(V4L2_PIX_FMT_NV12, 640, 480), "S_FMT: %s", std::strerror(errno));
    CHECK(node.width == 640 && node.height == 480 && node.stride == 640 && node.sizeimage == 640 * 720,
          "S_FMT 640x480: %ux%u stride %u image %u", node.width, node.height, node.stride, node.sizeimage);

    // The frame interval is the device's: a rate it has, whatever was asked.
    v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 15;
    CHECK(xioctl(node.fd, VIDIOC_S_PARM, &parm) == 0, "S_PARM: %s", std::strerror(errno));
    CHECK(parm.parm.capture.timeperframe.numerator == 1 && parm.parm.capture.timeperframe.denominator >= 1,
          "S_PARM answered %u/%u", parm.parm.capture.timeperframe.numerator, parm.parm.capture.timeperframe.denominator);
    v4l2_streamparm got {};
    got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    CHECK(xioctl(node.fd, VIDIOC_G_PARM, &got) == 0 &&
              got.parm.capture.timeperframe.denominator == parm.parm.capture.timeperframe.denominator,
          "G_PARM differs from S_PARM");

    // Still formats.
    CHECK(node.set_format(V4L2_PIX_FMT_JPEG, smallest(frame_sizes(node.fd, V4L2_PIX_FMT_JPEG)).first,
                          smallest(frame_sizes(node.fd, V4L2_PIX_FMT_JPEG)).second),
          "S_FMT JPEG: %s", std::strerror(errno));
    CHECK(node.pixelformat == V4L2_PIX_FMT_JPEG && node.sizeimage > 65000, "JPEG layout: image %u", node.sizeimage);

    // The metadata node has the one format.
    Node meta(camera.meta_path(), true);
    if (!meta.open_file()) return;
    v4l2_fmtdesc meta_format {};
    meta_format.type = V4L2_BUF_TYPE_META_CAPTURE;
    CHECK(xioctl(meta.fd, VIDIOC_ENUM_FMT, &meta_format) == 0 && meta_format.pixelformat == kMetaFormat, "metadata format");
    meta_format.index = 1;
    CHECK(xioctl(meta.fd, VIDIOC_ENUM_FMT, &meta_format) < 0, "a second metadata format");
    CHECK(meta.get_format() && meta.pixelformat == kMetaFormat && meta.sizeimage == sizeof(CameraMeta),
          "metadata layout: %u bytes", meta.sizeimage);
}

// Open creates no host stream: the host allows 32 streams, a node 40 files.
void test_many_opens(const Camera& camera)
{
    std::vector<int> files;
    for (int count = 0; count < 40; ++count) {
        const int fd = ::open(camera.capture_path().c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            CHECK(false, "open %d of %s: %s", count, camera.capture_path().c_str(), std::strerror(errno));
            break;
        }
        files.push_back(fd);
    }
    CHECK(files.size() == 40, "%zu files opened", files.size());
    if (!files.empty()) {
        // The last file still gets a stream.
        Node node(camera.capture_path(), false);
        node.fd = files.back();
        files.pop_back();
        CHECK(node.set_format(V4L2_PIX_FMT_NV12, 640, 480) && node.allocate(2, V4L2_MEMORY_MMAP) && node.queue_all(),
              "a stream after 40 opens: %s", std::strerror(errno));
        node.close_file();
    }
    for (const int fd : files) ::close(fd);
}

// ---------------------------------------------------------------------------
// Video streams
// ---------------------------------------------------------------------------

struct VideoStats {
    int frames = 0;
    int valid = 0;
    int black = 0;
    int failed = 0;
    std::uint16_t tag = 0;
    std::uint32_t last_counter = 0;
    std::uint32_t last_sequence = 0;
    std::vector<std::uint32_t> sequences;
};

// Dequeues `want` frames that carry the chart (or, blocked, black ones) and
// checks each against the layout and the clock; every buffer is queued again.
bool run_video(Node& node, int want, VideoStats& stats, const char* label, bool chart = true)
{
    constexpr int limit_extra = 90;
    std::uint64_t last_time = 0;
    int accepted = 0;
    for (int taken = 0; taken < want + limit_extra && accepted < want; ++taken) {
        Dequeued frame;
        const int result = node.dequeue(frame, 15000);
        if (result <= 0) {
            CHECK(false, "%s: no frame within 15 s after %d (%s)", label, taken, result < 0 ? std::strerror(errno) : "timeout");
            return false;
        }
        ++stats.frames;
        if (frame.flags & V4L2_BUF_FLAG_ERROR) {
            ++stats.failed;
            CHECK(false, "%s: frame %u failed", label, frame.index);
            node.queue(frame.index);
            continue;
        }
        CHECK(frame.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, "%s: frame lacks the monotonic timestamp flag", label);
        CHECK(frame.bytesused == node.sizeimage, "%s: frame of %u bytes, not %u", label, frame.bytesused, node.sizeimage);
        CHECK(stats.last_sequence == 0 || frame.sequence > stats.last_sequence, "%s: sequence %u after %u", label, frame.sequence,
              stats.last_sequence);
        stats.last_sequence = frame.sequence;
        stats.sequences.push_back(frame.sequence);
        const std::uint64_t stamp = std::uint64_t(frame.timestamp.tv_sec) * 1000000000ull + std::uint64_t(frame.timestamp.tv_usec) * 1000ull;
        const std::uint64_t now = monotonic_ns();
        CHECK(stamp <= now + 100000000ull && stamp + 30000000000ull >= now, "%s: timestamp %llu at %llu", label,
              static_cast<unsigned long long>(stamp), static_cast<unsigned long long>(now));
        CHECK(stamp >= last_time, "%s: timestamp went back", label);
        last_time = std::max(last_time, stamp);

        const std::uint8_t* data = node.data(frame.index);
        if (nv12_black(data, node.stride, node.width, node.height)) {
            ++stats.black;
            // Black stand-ins come first in a live session; a blocked one has nothing else.
            if (blocked_mode) ++accepted;
        } else {
            const Marker marker = decode_marker(data, node.stride, node.width, node.height);
            if (!chart) {
                // A size of another shape crops the marker away: it is a picture, not the chart.
                CHECK(nv12_has_content(data, node.stride, node.width, node.height), "%s: the frame has no picture", label);
                ++stats.valid;
                ++accepted;
            } else if (marker.valid && !blocked_mode) {
                ++stats.valid;
                CHECK((marker.tag & 0xff00) == 0x5a00, "%s: chart tag %#x", label, marker.tag);
                if (stats.valid > 1) {
                    CHECK(marker.tag == stats.tag, "%s: the chart tag changed to %#x", label, marker.tag);
                    CHECK(marker.counter > stats.last_counter, "%s: chart counter %u after %u", label, marker.counter,
                          stats.last_counter);
                }
                stats.tag = marker.tag;
                stats.last_counter = marker.counter;
                CHECK(nv12_has_content(data, node.stride, node.width, node.height), "%s: the frame has no picture", label);
                ++accepted;
            } else {
                CHECK(false, "%s: frame %u is neither black nor the chart (marker %s)", label, frame.index,
                      marker.valid ? "valid" : "invalid");
            }
        }
        if (!node.queue(frame.index)) {
            CHECK(false, "%s: QBUF: %s", label, std::strerror(errno));
            return false;
        }
    }
    CHECK(accepted >= want, "%s: %d of %d frames", label, accepted, want);
    return accepted >= want;
}

bool start_video(Node& node, std::uint32_t width, std::uint32_t height, unsigned buffers, v4l2_memory memory)
{
    if (!node.open_file()) return false;
    if (!node.set_format(V4L2_PIX_FMT_NV12, width, height) || node.width != width || node.height != height) {
        CHECK(false, "S_FMT NV12 %ux%u: %s", width, height, std::strerror(errno));
        return false;
    }
    if (!node.allocate(buffers, memory)) {
        CHECK(false, "REQBUFS: %s", std::strerror(errno));
        return false;
    }
    return true;
}

// The fastest rate the camera offers for a size, in frames a second.
unsigned fastest_rate(int fd, std::uint32_t width, std::uint32_t height)
{
    unsigned best = 0;
    for (std::uint32_t index = 0; index < 64; ++index) {
        v4l2_frmivalenum interval {};
        interval.index = index;
        interval.pixel_format = V4L2_PIX_FMT_NV12;
        interval.width = width;
        interval.height = height;
        if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) break;
        const v4l2_fract& shortest =
            interval.type == V4L2_FRMIVAL_TYPE_DISCRETE ? interval.discrete : interval.stepwise.min;
        if (shortest.numerator > 0) best = std::max(best, shortest.denominator / shortest.numerator);
        if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
    }
    return best;
}

// A file with a stream keeps the frame interval the device answers, not the one
// it asked for: a rate beyond the camera's, and one far below it, are brought to
// rates it has, and G_PARM says the same.
void test_frame_interval(const Camera& camera)
{
    Node node(camera.capture_path(), false);
    if (!start_video(node, 640, 480, 3, V4L2_MEMORY_MMAP)) return;
    const unsigned best = fastest_rate(node.fd, 640, 480);
    CHECK(best >= 15, "the camera offers %u frames a second at 640x480", best);
    const std::uint32_t asked[][2] = {{1, 100000}, {1000, 1}};
    for (const auto& ask : asked) {
        v4l2_streamparm parm {};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        parm.parm.capture.timeperframe.numerator = ask[0];
        parm.parm.capture.timeperframe.denominator = ask[1];
        CHECK(xioctl(node.fd, VIDIOC_S_PARM, &parm) == 0, "S_PARM %u/%u: %s", ask[0], ask[1], std::strerror(errno));
        const v4l2_fract answer = parm.parm.capture.timeperframe;
        CHECK(answer.numerator == 1 && answer.denominator >= 1 && answer.denominator <= best,
              "S_PARM %u/%u answered %u/%u (the camera's fastest is %u)", ask[0], ask[1], answer.numerator, answer.denominator,
              best);
        v4l2_streamparm got {};
        got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        CHECK(xioctl(node.fd, VIDIOC_G_PARM, &got) == 0 && got.parm.capture.timeperframe.numerator == answer.numerator &&
                  got.parm.capture.timeperframe.denominator == answer.denominator,
              "G_PARM says %u/%u after S_PARM answered %u/%u", got.parm.capture.timeperframe.numerator,
              got.parm.capture.timeperframe.denominator, answer.numerator, answer.denominator);
    }
}

void test_video_mmap(const Camera& camera, std::uint16_t& tag)
{
    Node node(camera.capture_path(), false);
    if (!start_video(node, 1280, 720, 4, V4L2_MEMORY_MMAP)) return;
    CHECK(node.stride == 1280 && node.sizeimage == 1280 * 1080, "layout %u / %u", node.stride, node.sizeimage);
    CHECK(node.queue_all() && node.stream_on(), "start: %s", std::strerror(errno));
    VideoStats stats;
    run_video(node, 8, stats, "mmap");
    tag = stats.tag;
    if (blocked_mode) {
        CHECK(stats.black == stats.frames && stats.valid == 0, "blocked: %d black of %d frames, %d with the chart", stats.black,
              stats.frames, stats.valid);
    } else {
        std::printf("camera %u: chart tag %#x, %d frames (%d black stand-ins)\n", camera.index, stats.tag, stats.frames, stats.black);
    }
    CHECK(node.stream_off(), "STREAMOFF: %s", std::strerror(errno));
    // A buffer that was queued again is not left with the device.
    Dequeued left;
    CHECK(node.dequeue(left, 0) != 1, "a buffer is left after STREAMOFF");
}

// Two files of one node stream together, each at its own size.
void test_two_files(const Camera& camera)
{
    Node first(camera.capture_path(), false);
    Node second(camera.capture_path(), false);
    if (!start_video(first, 1280, 720, 4, V4L2_MEMORY_MMAP) || !start_video(second, 640, 480, 3, V4L2_MEMORY_MMAP)) return;
    CHECK(first.queue_all() && first.stream_on(), "first file: %s", std::strerror(errno));
    CHECK(second.queue_all() && second.stream_on(), "second file: %s", std::strerror(errno));
    VideoStats a, b;
    run_video(first, 6, a, "file 1");
    // The second file's 4:3 frames can be cut from a 16:9 picture, which crops the sides of the chart's marker.
    run_video(second, 6, b, "file 2", false);
    run_video(first, 3, a, "file 1 again");
    CHECK(first.stream_off() && second.stream_off(), "STREAMOFF");
}

// A buffer another file exported is the memory of this file's frames.
void test_dmabuf(const Camera& camera)
{
    Node exporter(camera.capture_path(), false);
    Node importer(camera.capture_path(), false);
    if (!start_video(exporter, 640, 480, 3, V4L2_MEMORY_MMAP) || !start_video(importer, 640, 480, 3, V4L2_MEMORY_DMABUF)) return;
    if (!exporter.export_buffers()) {
        CHECK(false, "EXPBUF: %s", std::strerror(errno));
        return;
    }
    // The importer's frames are read through the exporter's mappings, and each
    // exported dma-buf must show the same memory.
    importer.buffers = exporter.buffers;
    for (auto& buffer : importer.buffers) buffer.owned_mapping = false;
    CHECK(importer.queue_all() && importer.stream_on(), "DMABUF start: %s", std::strerror(errno));
    VideoStats stats;
    run_video(importer, 6, stats, "dmabuf");
    CHECK(importer.stream_off(), "STREAMOFF");
    // Nothing writes now: every exported dma-buf shows the memory its file maps.
    for (std::size_t index = 0; index < exporter.buffers.size(); ++index) {
        void* view = mmap(nullptr, exporter.buffers[index].length, PROT_READ, MAP_SHARED, exporter.buffers[index].dma_fd, 0);
        if (view == MAP_FAILED) {
            CHECK(false, "mmap of dma-buf %zu: %s", index, std::strerror(errno));
            continue;
        }
        CHECK(std::memcmp(view, exporter.buffers[index].address, exporter.buffers[index].length) == 0,
              "dma-buf %zu differs from its mapping", index);
        munmap(view, exporter.buffers[index].length);
    }
    importer.buffers.clear();
}

// ---------------------------------------------------------------------------
// The metadata node
// ---------------------------------------------------------------------------

struct MetaStream {
    Node node;
    std::vector<CameraMeta> records;
    // The indices of the buffers that came back, however they came.
    std::set<unsigned> returned;
    explicit MetaStream(const Camera& camera) : node(camera.meta_path(), true) {}

    bool start()
    {
        if (!node.open_file()) return false;
        if (!node.allocate(16, V4L2_MEMORY_MMAP)) {
            CHECK(false, "metadata REQBUFS: %s", std::strerror(errno));
            return false;
        }
        CHECK(node.queue_all() && node.stream_on(), "metadata start: %s", std::strerror(errno));
        return true;
    }

    // Takes what is ready.
    void collect(int timeout_ms)
    {
        Dequeued record;
        int result = node.dequeue(record, timeout_ms);
        while (result > 0) {
            returned.insert(record.index);
            if (record.flags & V4L2_BUF_FLAG_ERROR) {
                CHECK(false, "a metadata buffer failed");
            } else if (record.bytesused != sizeof(CameraMeta)) {
                CHECK(false, "a metadata record of %u bytes", record.bytesused);
            } else {
                CameraMeta meta;
                std::memcpy(&meta, node.data(record.index), sizeof(meta));
                records.push_back(meta);
            }
            node.queue(record.index);
            result = node.dequeue(record, 0);
        }
    }
};

void command_blocked(int)
{
    static const char text[] = "FAIL: a command blocked with the buffer budget full\n";
    (void)!::write(STDOUT_FILENO, text, sizeof(text) - 1);
    ::_exit(1);
}

// The device holds at most 96 buffers (the driver's VIRTIO_VIDEO_CAMERA_MAX_QUEUED)
// of every kind and camera, so that queued buffers can never take the command
// queue's whole ring. Metadata streams with no exposure to fill them hold their
// buffers for good: eight files of sixteen, the surplus of the first ninety-six
// is refused as failed buffers at once, a command still completes with the
// budget full, and the room is there again when the streams stop.
void test_buffer_budget(const Camera& camera)
{
    constexpr unsigned budget = 96, per_file = 16, files = budget / per_file + 2;
    std::vector<std::unique_ptr<Node>> nodes;
    for (unsigned i = 0; i < files; ++i) {
        nodes.push_back(std::make_unique<Node>(camera.meta_path(), true));
        if (!nodes.back()->open_file() || !nodes.back()->allocate(per_file, V4L2_MEMORY_MMAP)) {
            CHECK(false, "metadata file %u: %s", i, std::strerror(errno));
            return;
        }
    }
    for (auto& node : nodes) CHECK(node->queue_all() && node->stream_on(), "metadata start: %s", std::strerror(errno));

    unsigned refused = 0, answered = 0;
    for (auto& node : nodes) {
        Dequeued buffer;
        while (node->dequeue(buffer, 0) > 0) {
            if (buffer.flags & V4L2_BUF_FLAG_ERROR) ++refused;
            else ++answered;
        }
    }
    CHECK(refused == files * per_file - budget, "%u of %u metadata buffers were refused, not %u", refused, files * per_file,
          files * per_file - budget);
    CHECK(answered == 0, "%u metadata buffers came with no exposure", answered);

    // A command goes through with the budget full: the zoom is written and read back.
    Node capture(camera.capture_path(), false);
    std::signal(SIGALRM, command_blocked);
    alarm(60);
    const bool opened = capture.open_file();
    const bool wrote = opened && set_control(capture.fd, kCidZoomQ16, 0x20000);
    alarm(0);
    std::signal(SIGALRM, SIG_DFL);
    CHECK(wrote, "the zoom write with the budget full: %s", std::strerror(errno));
    std::int64_t zoom = 0;
    CHECK(opened && get_control(capture.fd, kCidZoomQ16, zoom) && zoom == 0x20000, "the zoom reads %#llx", static_cast<long long>(zoom));
    if (opened) set_control(capture.fd, kCidZoomQ16, 0x10000);

    for (auto& node : nodes) CHECK(node->stream_off(), "STREAMOFF: %s", std::strerror(errno));
    nodes.clear();

    // The room is back: a fresh file's buffers are all accepted.
    Node again(camera.meta_path(), true);
    if (!again.open_file() || !again.allocate(per_file, V4L2_MEMORY_MMAP)) {
        CHECK(false, "metadata file after the budget: %s", std::strerror(errno));
        return;
    }
    CHECK(again.queue_all() && again.stream_on(), "metadata start after the budget: %s", std::strerror(errno));
    unsigned failed = 0;
    Dequeued buffer;
    while (again.dequeue(buffer, 0) > 0) ++failed;
    CHECK(failed == 0, "%u of a fresh file's metadata buffers were refused after the streams stopped", failed);
    CHECK(again.stream_off(), "STREAMOFF: %s", std::strerror(errno));
}

// Reads video and metadata together until `done` accepts a record or the time is up.
template <typename Done>
bool pump(Node& video, MetaStream& meta, VideoStats* stats, std::vector<CameraMeta>& seen, Done done, int seconds)
{
    const std::uint64_t deadline = monotonic_ns() + std::uint64_t(seconds) * 1000000000ull;
    std::size_t examined = 0;
    while (monotonic_ns() < deadline) {
        Dequeued frame;
        while (video.dequeue(frame, 0) > 0) {
            if (stats != nullptr) {
                ++stats->frames;
                stats->sequences.push_back(frame.sequence);
            }
            video.queue(frame.index);
        }
        meta.collect(100);
        for (; examined < meta.records.size(); ++examined) {
            seen.push_back(meta.records[examined]);
            if (done(meta.records[examined])) return true;
        }
    }
    return false;
}

void test_meta(const Camera& camera)
{
    MetaStream meta(camera);
    Node video(camera.capture_path(), false);
    if (!meta.start() || !start_video(video, 1280, 720, 4, V4L2_MEMORY_MMAP)) return;
    CHECK(video.queue_all() && video.stream_on(), "video start: %s", std::strerror(errno));

    VideoStats stats;
    std::vector<CameraMeta> seen;
    CHECK(pump(video, meta, &stats, seen, [&](const CameraMeta&) { return seen.size() >= 20; }, 30), "%zu metadata records", seen.size());

    // Frames and records carry the exposure's sequence.
    std::set<std::uint64_t> record_sequences;
    for (const auto& record : seen) record_sequences.insert(record.sequence);
    int matched = 0;
    for (const auto sequence : stats.sequences) matched += record_sequences.count(sequence);
    CHECK(matched >= 5, "%d of %zu frames have a metadata record", matched, stats.sequences.size());
    std::uint64_t previous = 0;
    for (const auto& record : seen) {
        CHECK(record.sequence > previous, "record sequence %llu after %llu", static_cast<unsigned long long>(record.sequence),
              static_cast<unsigned long long>(previous));
        previous = record.sequence;
        CHECK(!(record.flags & kFrameStill), "a still's record on a video stream");
        if (blocked_mode) {
            CHECK((record.flags & kFrameBlack) && (record.flags & kFrameBlocked), "record flags %#x are not black and blocked",
                  record.flags);
        } else if (!(record.flags & kFrameBlack)) {
            CHECK(record.exposure_ns > 0 && record.frame_duration_ns > 0 && record.iso > 0, "record without exposure: %llu ns, ISO %u",
                  static_cast<unsigned long long>(record.exposure_ns), record.iso);
            if (camera.have_desc) CHECK(record.active_camera == camera.desc.index, "record of camera %u", record.active_camera);
        }
    }
    if (blocked_mode) return;

    // A volatile control is read from the camera at each read. A camera that
    // focuses by itself has one, the status of its focus: once frames flow, it
    // says the focus has been reached, which a driver that answered with the
    // control's default (idle) would not.
    if (find_control(enumerate_controls(video.fd), V4L2_CID_AUTO_FOCUS_STATUS) != nullptr) {
        std::int64_t status = -1;
        const bool reached = pump(video, meta, nullptr, seen,
                                  [&](const CameraMeta&) {
                                      return get_control(video.fd, V4L2_CID_AUTO_FOCUS_STATUS, status) &&
                                             status == V4L2_AUTO_FOCUS_STATUS_REACHED;
                                  },
                                  30);
        CHECK(reached, "the focus status reads %lld, not reached", static_cast<long long>(status));
    }

    // A control the guest writes reaches the camera and shows in the records.
    CHECK(set_control(video.fd, kCidZoomQ16, 0x20000), "zoom 2.0: %s", std::strerror(errno));
    std::int64_t zoom = 0;
    CHECK(get_control(video.fd, kCidZoomQ16, zoom) && zoom == 0x20000, "zoom reads back %lld", static_cast<long long>(zoom));
    seen.clear();
    CHECK(pump(video, meta, nullptr, seen, [](const CameraMeta& record) { return record.zoom_ratio_q16 == 0x20000; }, 30),
          "no record shows zoom 2.0 (last %#x)", seen.empty() ? 0u : seen.back().zoom_ratio_q16);
    CHECK(set_control(video.fd, kCidZoomQ16, 0x10000), "zoom 1.0");
    seen.clear();
    CHECK(pump(video, meta, nullptr, seen, [](const CameraMeta& record) { return record.zoom_ratio_q16 == 0x10000; }, 30),
          "no record shows zoom 1.0 again");

    if (camera.have_desc && camera.desc.flags & kCameraManualExposure) {
        const std::int64_t exposure = 4000000;
        const std::int64_t iso = std::min<std::int64_t>(std::max<std::int64_t>(200, camera.desc.iso_min), camera.desc.iso_max);
        CHECK(set_control(video.fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL), "manual exposure: %s", std::strerror(errno));
        CHECK(set_control(video.fd, kCidExposureNs, exposure, true), "exposure time: %s", std::strerror(errno));
        CHECK(set_control(video.fd, kCidIso, iso), "ISO: %s", std::strerror(errno));
        std::int64_t read_back = 0;
        CHECK(get_control(video.fd, kCidExposureNs, read_back, true) && read_back == exposure, "exposure reads back %lld",
              static_cast<long long>(read_back));
        seen.clear();
        CHECK(pump(video, meta, nullptr, seen,
                   [&](const CameraMeta& record) { return std::int64_t(record.exposure_ns) == exposure && std::int64_t(record.iso) == iso; },
                   30),
              "no record shows %lld ns at ISO %lld (last %llu ns, ISO %u)", static_cast<long long>(exposure),
              static_cast<long long>(iso), static_cast<unsigned long long>(seen.empty() ? 0 : seen.back().exposure_ns),
              seen.empty() ? 0u : seen.back().iso);
        CHECK(set_control(video.fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO), "automatic exposure again");
    }
    CHECK(video.stream_off() && meta.node.stream_off(), "STREAMOFF");
}

// Two metadata files of one camera stream at once, beside its video: each gets
// the records of the exposures in order, and the two see the same exposures.
void test_two_meta(const Camera& camera)
{
    MetaStream first(camera), second(camera);
    Node video(camera.capture_path(), false);
    if (!first.start() || !second.start() || !start_video(video, 1280, 720, 4, V4L2_MEMORY_MMAP)) return;
    CHECK(video.queue_all() && video.stream_on(), "video start: %s", std::strerror(errno));
    const std::uint64_t deadline = monotonic_ns() + 30ull * 1000000000ull;
    while (monotonic_ns() < deadline && (first.records.size() < 20 || second.records.size() < 20)) {
        Dequeued frame;
        while (video.dequeue(frame, 0) > 0) video.queue(frame.index);
        first.collect(50);
        second.collect(50);
    }
    CHECK(first.records.size() >= 20 && second.records.size() >= 20, "%zu and %zu records on two metadata streams",
          first.records.size(), second.records.size());
    std::set<std::uint64_t> first_sequences;
    for (const auto* stream : {&first, &second}) {
        std::uint64_t previous = 0;
        for (const auto& record : stream->records) {
            CHECK(record.sequence > previous, "record sequence %llu after %llu on one of two streams",
                  static_cast<unsigned long long>(record.sequence), static_cast<unsigned long long>(previous));
            previous = record.sequence;
            if (stream == &first) first_sequences.insert(record.sequence);
        }
    }
    int shared = 0;
    for (const auto& record : second.records) shared += first_sequences.count(record.sequence);
    CHECK(shared >= 10, "only %d of %zu records of the second stream are of an exposure the first one has", shared,
          second.records.size());
    CHECK(video.stream_off() && first.node.stream_off() && second.node.stream_off(), "STREAMOFF");
}

// ---------------------------------------------------------------------------
// Stills
// ---------------------------------------------------------------------------

struct Still {
    Node node;
    Still(const Camera& camera) : node(camera.capture_path(), false) {}

    bool start(std::uint32_t fourcc)
    {
        if (!node.open_file()) return false;
        const auto sizes = smallest(frame_sizes(node.fd, fourcc));
        if (sizes.first == 0) return false;
        if (!node.set_format(fourcc, sizes.first, sizes.second) || node.pixelformat != fourcc) {
            CHECK(false, "S_FMT %.4s: %s", reinterpret_cast<const char*>(&fourcc), std::strerror(errno));
            return false;
        }
        if (!node.allocate(2, V4L2_MEMORY_MMAP) || !node.stream_on()) {
            CHECK(false, "still start: %s", std::strerror(errno));
            return false;
        }
        return true;
    }
};

bool is_bayer(std::uint32_t fourcc)
{
    return fourcc == V4L2_PIX_FMT_SRGGB16 || fourcc == V4L2_PIX_FMT_SGRBG16 || fourcc == V4L2_PIX_FMT_SGBRG16 ||
           fourcc == V4L2_PIX_FMT_SBGGR16;
}

std::uint32_t find_bayer(const Camera& camera)
{
    Node node(camera.capture_path(), false);
    if (!node.open_file()) return 0;
    for (std::uint32_t index = 0; index < 16; ++index) {
        v4l2_fmtdesc description {};
        description.index = index;
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(node.fd, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (is_bayer(description.pixelformat)) return description.pixelformat;
    }
    return 0;
}

void check_jpeg(const Node& node, const Dequeued& still, const char* label)
{
    const std::uint8_t* data = node.data(still.index);
    CHECK(still.bytesused > 1000 && still.bytesused <= node.sizeimage, "%s: JPEG of %u bytes", label, still.bytesused);
    if (still.bytesused < 4) return;
    CHECK(data[0] == 0xff && data[1] == 0xd8, "%s: no SOI", label);
    CHECK(data[still.bytesused - 2] == 0xff && data[still.bytesused - 1] == 0xd9, "%s: no EOI at the end", label);
}

void check_bayer(const Node& node, const Dequeued& still, const Camera& camera, const char* label)
{
    CHECK(still.bytesused == node.sizeimage && node.stride >= 2 * node.width, "%s: %u bytes, stride %u", label, still.bytesused,
          node.stride);
    const std::uint8_t* data = node.data(still.index);
    std::uint32_t low = 0xffff, high = 0;
    for (std::uint32_t y = 0; y < node.height; y += 3)
        for (std::uint32_t x = 0; x < node.width; x += 3) {
            const std::uint8_t* sample = data + std::size_t(y) * node.stride + std::size_t(x) * 2;
            const std::uint32_t value = sample[0] | (std::uint32_t(sample[1]) << 8);
            low = std::min(low, value);
            high = std::max(high, value);
        }
    std::printf("%s: samples %u..%u\n", label, low, high);
    CHECK(high > low + 100, "%s: the samples barely vary", label);
    const std::uint32_t white = camera.have_desc && camera.desc.white_level ? camera.desc.white_level : 16383;
    CHECK(high <= white, "%s: a sample of %u above the white level %u", label, high, white);
}

void test_stills(const Camera& camera)
{
    const std::uint32_t bayer = find_bayer(camera);
    std::printf("camera %u: Bayer format %.4s\n", camera.index, bayer ? reinterpret_cast<const char*>(&bayer) : "none");
    // A blocked session never captures, so it never learns the colour data a Bayer still needs.
    if (!blocked_mode && camera.have_desc && camera.back()) CHECK(bayer != 0, "the back camera offers no Bayer still");

    MetaStream meta(camera);
    const bool with_meta = !blocked_mode && meta.start();
    std::vector<CameraMeta> seen;
    Node idle(camera.capture_path(), false);

    // A single JPEG.
    Still jpeg(camera);
    if (!jpeg.start(V4L2_PIX_FMT_JPEG)) return;
    CHECK(jpeg.node.queue(0), "QBUF: %s", std::strerror(errno));
    Dequeued still;
    const int result = jpeg.node.dequeue(still, 30000);
    CHECK(result > 0, "JPEG still: %s", result == 0 ? "timeout" : std::strerror(errno));
    if (result <= 0) return;
    if (blocked_mode) {
        CHECK(still.flags & V4L2_BUF_FLAG_ERROR, "a blocked session's still is not a failed buffer");
        CHECK(still.bytesused == 0, "a failed still carries %u bytes", still.bytesused);
        return;
    }
    CHECK(!(still.flags & V4L2_BUF_FLAG_ERROR), "the JPEG still failed");
    check_jpeg(jpeg.node, still, "single JPEG");
    if (with_meta) {
        const std::uint64_t want = still.sequence;
        meta.collect(2000);
        seen = meta.records;
        bool found = false;
        for (int round = 0; round < 50 && !found; ++round) {
            for (const auto& record : meta.records)
                if (record.sequence == want && (record.flags & kFrameStill)) found = true;
            if (!found) meta.collect(200);
        }
        CHECK(found, "no still record with sequence %llu on the metadata node", static_cast<unsigned long long>(want));
    }

    // Another still on the same stream is another exposure.
    jpeg.node.queue(still.index);
    Dequeued second;
    CHECK(jpeg.node.dequeue(second, 30000) > 0 && second.sequence > still.sequence, "a second still: sequence %u after %u",
          second.sequence, still.sequence);

    // A single Bayer still.
    Still raw(camera);
    if (bayer != 0 && raw.start(bayer)) {
        CHECK(raw.node.queue(0), "QBUF Bayer: %s", std::strerror(errno));
        Dequeued bayer_still;
        const int got = raw.node.dequeue(bayer_still, 30000);
        CHECK(got > 0 && !(bayer_still.flags & V4L2_BUF_FLAG_ERROR), "the Bayer still failed");
        if (got > 0) check_bayer(raw.node, bayer_still, camera, "single Bayer");
        raw.node.queue(bayer_still.index);
    }

    // A group: the JPEG and the Bayer still of one cookie share one exposure.
    Still group_jpeg(camera);
    Still group_raw(camera);
    if (bayer != 0 && group_jpeg.start(V4L2_PIX_FMT_JPEG) && group_raw.start(bayer)) {
        CHECK(group_jpeg.node.queue(0, 77, false), "group QBUF (JPEG): %s", std::strerror(errno));
        CHECK(group_raw.node.queue(0, 77, true), "group QBUF (Bayer): %s", std::strerror(errno));
        Dequeued a, b;
        const int ra = group_jpeg.node.dequeue(a, 30000);
        const int rb = group_raw.node.dequeue(b, 30000);
        CHECK(ra > 0 && rb > 0, "group completion: %d %d", ra, rb);
        if (ra > 0 && rb > 0) {
            CHECK(!(a.flags & V4L2_BUF_FLAG_ERROR) && !(b.flags & V4L2_BUF_FLAG_ERROR), "a group member failed");
            CHECK(a.sequence == b.sequence, "the group's stills are of exposures %u and %u", a.sequence, b.sequence);
            check_jpeg(group_jpeg.node, a, "group JPEG");
            check_bayer(group_raw.node, b, camera, "group Bayer");
        }
    }
}

// ---------------------------------------------------------------------------
// The torch needs no stream
// ---------------------------------------------------------------------------

void test_torch(const Camera& camera)
{
    if (!camera.have_desc || !(camera.desc.flags & kCameraTorch)) return;
    Node node(camera.capture_path(), false);
    if (!node.open_file()) return;
    CHECK(set_control(node.fd, V4L2_CID_FLASH_LED_MODE, V4L2_FLASH_LED_MODE_TORCH), "torch on: %s", std::strerror(errno));
    std::int64_t mode = -1;
    CHECK(get_control(node.fd, V4L2_CID_FLASH_LED_MODE, mode) && mode == V4L2_FLASH_LED_MODE_TORCH, "torch reads %lld",
          static_cast<long long>(mode));
    // Another file of the camera sees the same torch.
    Node other(camera.meta_path(), true);
    if (other.open_file())
        CHECK(get_control(other.fd, V4L2_CID_FLASH_LED_MODE, mode) && mode == V4L2_FLASH_LED_MODE_TORCH, "torch reads %lld on the metadata node",
              static_cast<long long>(mode));
    CHECK(set_control(node.fd, V4L2_CID_FLASH_LED_MODE, V4L2_FLASH_LED_MODE_NONE), "torch off: %s", std::strerror(errno));
    CHECK(get_control(node.fd, V4L2_CID_FLASH_LED_MODE, mode) && mode == V4L2_FLASH_LED_MODE_NONE, "torch reads %lld after off",
          static_cast<long long>(mode));
    // The menu runs from none to torch, and the camera skips the flash in it: an
    // entry the menu skips is refused (and does not show in QUERYMENU), though
    // it lies inside the range; so is one outside the range. Neither moves the
    // value.
    for (std::uint32_t entry = V4L2_FLASH_LED_MODE_NONE; entry <= V4L2_FLASH_LED_MODE_TORCH; ++entry) {
        v4l2_querymenu item {};
        item.id = V4L2_CID_FLASH_LED_MODE;
        item.index = entry;
        const bool listed = xioctl(node.fd, VIDIOC_QUERYMENU, &item) == 0;
        CHECK(listed == (entry != V4L2_FLASH_LED_MODE_FLASH), "the flash mode menu %s entry %u",
              listed ? "lists" : "skips", entry);
    }
    CHECK(!set_control(node.fd, V4L2_CID_FLASH_LED_MODE, V4L2_FLASH_LED_MODE_FLASH), "a flash mode the menu skips was accepted");
    CHECK(!set_control(node.fd, V4L2_CID_FLASH_LED_MODE, 9), "a torch mode outside the menu was accepted");
    CHECK(get_control(node.fd, V4L2_CID_FLASH_LED_MODE, mode) && mode == V4L2_FLASH_LED_MODE_NONE,
          "a refused torch mode moved the value to %lld", static_cast<long long>(mode));
}

// The host learns a camera's colour data from the first Bayer still it takes and
// keeps it for the next session, whose cameras then offer Bayer stills. This
// takes that still: one JPEG on the back camera.
int calibrate()
{
    step("calibrate");
    auto cameras = discover();
    bool taken = false;
    for (auto& camera : cameras) {
        Node node(camera.capture_path(), false);
        if (!node.open_file()) continue;
        camera.have_desc = read_description(node.fd, camera.desc);
        if (!camera.have_desc || !camera.back()) continue;
        Still jpeg(camera);
        if (!jpeg.start(V4L2_PIX_FMT_JPEG)) return failures ? failures : 1;
        CHECK(jpeg.node.queue(0), "QBUF: %s", std::strerror(errno));
        Dequeued still;
        const int result = jpeg.node.dequeue(still, 30000);
        CHECK(result > 0 && !(still.flags & V4L2_BUF_FLAG_ERROR), "the calibrating still");
        // The host saves what it learned as the still completes.
        sleep(2);
        taken = true;
    }
    CHECK(taken, "no back camera to take the calibrating still with");
    std::printf("UML_V4L2_CAPTURE: %s (calibrate)\n", failures == 0 ? "PASS" : "FAILED");
    return failures > 255 ? 255 : failures;
}

// A stream open across the guest's suspends.
int hold()
{
    constexpr std::uint32_t zoom = 0x20000;
    step("hold");
    auto cameras = discover();
    const Camera* back = nullptr;
    for (auto& camera : cameras) {
        Node node(camera.capture_path(), false);
        if (node.open_file()) camera.have_desc = read_description(node.fd, camera.desc);
        if (camera.have_desc && camera.back()) back = &camera;
    }
    CHECK(back != nullptr, "no back camera");
    if (back == nullptr) return failures ? failures : 1;

    Node video(back->capture_path(), false);
    MetaStream meta(*back);
    if (!start_video(video, 1280, 720, 4, V4L2_MEMORY_MMAP) || !meta.start()) return failures ? failures : 1;
    CHECK(video.queue_all() && video.stream_on(), "start: %s", std::strerror(errno));
    CHECK(set_control(video.fd, kCidZoomQ16, zoom), "zoom: %s", std::strerror(errno));

    std::printf("CAMSUSP ready\n");
    std::fflush(stdout);
    // A gap in the frames longer than this is a suspend: from there on the
    // buffers that come back are counted afresh (an epoch), so that a buffer the
    // driver parked and never queued again shows as one that never returns.
    constexpr std::uint64_t gap_ns = 2000000000ull;
    unsigned frames = 0, errors = 0, epoch = 0, regressions = 0;
    std::uint32_t last_sequence = 0;
    std::set<unsigned> video_returned;
    std::uint64_t last_report = 0, last_frame = 0;
    const std::uint64_t deadline = monotonic_ns() + 900ull * 1000000000ull;
    while (access("/tmp/camera-stop", F_OK) != 0 && monotonic_ns() < deadline) {
        Dequeued frame;
        const int result = video.dequeue(frame, 1000);
        if (result < 0) {
            CHECK(false, "DQBUF: %s", std::strerror(errno));
            break;
        }
        if (result > 0) {
            const std::uint64_t arrived = boottime_ns();
            if (last_frame != 0 && arrived - last_frame > gap_ns) {
                ++epoch;
                video_returned.clear();
                meta.returned.clear();
            }
            last_frame = arrived;
            ++frames;
            if (frame.flags & V4L2_BUF_FLAG_ERROR) ++errors;
            // The device counts again from zero when it is reset; the driver
            // carries the camera's sequence on.
            if (frames > 1 && frame.sequence <= last_sequence) ++regressions;
            last_sequence = frame.sequence;
            video_returned.insert(frame.index);
            video.queue(frame.index);
        }
        meta.collect(0);
        const std::uint64_t now = monotonic_ns();
        if (now - last_report >= 1000000000ull) {
            last_report = now;
            std::printf("CAMSUSP tick frames=%u errors=%u zoom=%#x epoch=%u video=%zu meta=%zu regressions=%u\n", frames, errors,
                        meta.records.empty() ? 0u : meta.records.back().zoom_ratio_q16, epoch, video_returned.size(),
                        meta.returned.size(), regressions);
            std::fflush(stdout);
            // The records need not pile up here.
            if (meta.records.size() > 64) meta.records.erase(meta.records.begin(), meta.records.end() - 1);
        }
    }
    CHECK(errors == 0, "%u failed buffers", errors);
    CHECK(regressions == 0, "%u frames whose sequence did not rise", regressions);
    std::int64_t value = 0;
    CHECK(get_control(video.fd, kCidZoomQ16, value) && value == zoom, "zoom reads %#llx", static_cast<long long>(value));
    CHECK(video.stream_off() && meta.node.stream_off(), "STREAMOFF");
    std::printf("CAMSUSP %s\n", failures == 0 ? "PASS" : "FAILED");
    return failures > 255 ? 255 : failures;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "synthetic") != 0 && std::strcmp(argv[1], "blocked") != 0 &&
                      std::strcmp(argv[1], "calibrate") != 0 && std::strcmp(argv[1], "hold") != 0)) {
        std::fprintf(stderr, "usage: %s synthetic|blocked|calibrate|hold\n", argv[0]);
        return 2;
    }
    blocked_mode = std::strcmp(argv[1], "blocked") == 0;
    if (std::strcmp(argv[1], "calibrate") == 0) return calibrate();
    if (std::strcmp(argv[1], "hold") == 0) return hold();

    step("nodes");
    auto cameras = discover();
    CHECK(int(cameras.size()) == kExpectedCameras, "%zu cameras, not %d", cameras.size(), kExpectedCameras);
    if (cameras.empty()) {
        std::printf("UML_V4L2_CAPTURE: FAILED (no camera nodes)\n");
        return failures ? failures : 1;
    }

    step("querycap");
    for (const auto& camera : cameras) test_querycap(camera);
    step("description");
    for (auto& camera : cameras) test_description(camera);
    step("controls");
    for (const auto& camera : cameras) test_controls(camera);
    step("formats");
    for (const auto& camera : cameras) test_formats(camera);
    step("many opens");
    test_many_opens(cameras.front());

    step("video");
    std::set<std::uint16_t> tags;
    for (const auto& camera : cameras) {
        std::uint16_t tag = 0;
        test_video_mmap(camera, tag);
        if (!blocked_mode) tags.insert(tag);
    }
    if (!blocked_mode) CHECK(tags.size() == cameras.size(), "the cameras show %zu different chart tags", tags.size());

    const Camera* back = nullptr;
    for (const auto& camera : cameras)
        if (camera.have_desc && camera.back()) back = &camera;
    CHECK(back != nullptr, "no back camera");
    if (back == nullptr) back = &cameras.front();

    step("torch with no stream");
    test_torch(*back);
    step("frame interval");
    for (const auto& camera : cameras) test_frame_interval(camera);
    step("two files on one node");
    test_two_files(*back);
    if (!blocked_mode) {
        step("dmabuf");
        test_dmabuf(*back);
    }
    step("metadata");
    test_meta(*back);
    step("two metadata streams");
    test_two_meta(*back);
    step("buffer budget");
    test_buffer_budget(*back);
    step("stills");
    test_stills(*back);

    if (failures == 0) {
        std::printf("UML_V4L2_CAPTURE: PASS (%s)\n", argv[1]);
        return 0;
    }
    std::printf("UML_V4L2_CAPTURE: FAILED (%d)\n", failures);
    return failures > 255 ? 255 : failures;
}
