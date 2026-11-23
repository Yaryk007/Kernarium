// SPDX-License-Identifier: GPL-2.0-or-later

// Vulkan asks KosmicKrisp for the last vertex of a primitive as its provoking
// vertex, which GL's default does for every draw, and Metal's is the first.
// The driver turns the draw into an indexed list of rotated primitives to
// bridge the two, which costs a heap, a compute dispatch and an indirect draw
// per draw. The rotation can change a result only where a value is taken from
// one vertex of the primitive: a fragment input that is not interpolated, and
// the layer or the viewport index. A draw whose pipeline has none of them
// renders what the first-vertex draw renders, and the driver does not unroll
// it.
//
// The contract under test, for triangle lists and strips:
//
//  - a flat fragment input takes the first vertex of each primitive under the
//    first-vertex convention and the last under the last-vertex one, and the
//    last-vertex draw is still unrolled (with the rewrite for the provoking
//    vertex);
//  - a pipeline whose fragment shader reads only interpolated inputs renders
//    the same pixels under both conventions and is not unrolled, though its
//    vertex shader writes a flat output nothing reads;
//  - a vertex shader that writes the layer picks it from the provoking vertex
//    under both conventions, and the last-vertex draw is unrolled.
//
// Why the pixels are the same: coverage is a function of the vertex
// positions, a cyclic rotation of a primitive keeps its winding, and an
// interpolated value is the barycentric combination of the vertex values, so
// no step of the arithmetic depends on which vertex is listed first. The
// smooth case holds that to account by comparing the whole image of the
// last-vertex draw with the first-vertex draw, which the driver used to
// rotate and now draws as it is; the images are equal on both.
//
// The driver says what it did through MESA_KK_DEBUG=unroll, one line per
// draw. The counts are checked on KosmicKrisp itself; any other driver (the
// Venus driver in an arm64 Linux guest) has no such line, and its pixels are
// checked all the same.

#include "vulkan_draw_test.hpp"
#include "shaders/provoking_unroll_spirv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace uml::gpu::test;

constexpr uint32_t size = 64;

// A failed check throws so that main can print it: uml::testing::require
// aborts, and its message would go to the stderr this test captures.
void require(bool condition, const char *what)
{
    if (!condition)
        throw std::runtime_error(what);
}

// What the process writes to stderr after a point, read back from a file.
class StderrCapture {
public:
    StderrCapture() : file_(std::tmpfile()), saved_(dup(STDERR_FILENO))
    {
        require(file_ != nullptr && saved_ >= 0, "stderr capture could not start");
        require(dup2(fileno(file_), STDERR_FILENO) >= 0, "stderr capture could not redirect");
    }
    ~StderrCapture()
    {
        std::fflush(stderr);
        if (saved_ >= 0) {
            dup2(saved_, STDERR_FILENO);
            close(saved_);
        }
        if (file_ != nullptr)
            std::fclose(file_);
    }
    StderrCapture(const StderrCapture &) = delete;
    StderrCapture &operator=(const StderrCapture &) = delete;

    // The text written since the last call.
    std::string take()
    {
        std::fflush(stderr);
        struct stat status{};
        require(fstat(fileno(file_), &status) == 0, "stderr capture could not be sized");
        std::string text(static_cast<size_t>(status.st_size) - consumed_, '\0');
        const ssize_t got = pread(fileno(file_), text.data(), text.size(),
                                  static_cast<off_t>(consumed_));
        require(got == static_cast<ssize_t>(text.size()), "stderr capture could not be read");
        consumed_ += text.size();
        return text;
    }

private:
    FILE *file_;
    int saved_;
    size_t consumed_ = 0;
};

struct Counts {
    unsigned draws = 0;
    unsigned unrolled = 0;
    unsigned rewrites = 0;
};

// Totals the "KK_UNROLL unrolled=U provoking_rewrite=R provoking_last=L"
// lines of `text`.
Counts count(const std::string &text)
{
    Counts counts;
    size_t position = 0;
    while ((position = text.find("KK_UNROLL ", position)) != std::string::npos) {
        const size_t end = text.find('\n', position);
        const std::string_view line(text.data() + position,
                                    (end == std::string::npos ? text.size() : end) - position);
        const auto field = [&](std::string_view name) {
            const size_t at = line.find(name);
            require(at != std::string_view::npos, "a KK_UNROLL line lacks a field");
            return line[at + name.size()] == '1';
        };
        ++counts.draws;
        counts.unrolled += field("unrolled=");
        counts.rewrites += field("provoking_rewrite=");
        position += line.size();
    }
    return counts;
}

enum class Shape { list, strip, layered };

struct Primitive {
    std::array<uint32_t, 3> vertices;
    unsigned x;
    unsigned y;
};

// The primitives of a draw and a pixel well inside each, from the geometry
// provoking_unroll.vert places.
std::vector<Primitive> primitives(Shape shape)
{
    std::vector<Primitive> result;
    switch (shape) {
    case Shape::list:
        for (uint32_t cell = 0; cell < 4; ++cell)
            result.push_back({{3 * cell, 3 * cell + 1, 3 * cell + 2},
                              (cell % 2) * 32 + 12, (cell / 2) * 32 + 12});
        break;
    case Shape::strip:
        for (uint32_t n = 0; n < 6; ++n)
            result.push_back({{n, n + 1, n + 2}, 12 + 8 * n, n % 2 == 0 ? 24u : 40u});
        break;
    case Shape::layered:
        result.push_back({{0, 1, 2}, 12, 12});
        break;
    }
    return result;
}

uint32_t vertex_count(Shape shape)
{
    return shape == Shape::list ? 12 : shape == Shape::strip ? 8 : 3;
}

struct Case {
    Shape shape;
    VkProvokingVertexModeEXT provoking;
    bool read_flat;
    bool dynamic = false;
    bool indexed = false;
};

DrawResult run(DrawContext &context, const Case &c)
{
    static const std::array<uint32_t, 12> indices{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    DrawConfig config;
    config.vertex_shader = c.shape == Shape::list    ? std::span<const uint32_t>(provoking_list_vertex)
                           : c.shape == Shape::strip ? std::span<const uint32_t>(provoking_strip_vertex)
                                                     : std::span<const uint32_t>(provoking_layer_vertex);
    config.fragment_shader = c.read_flat ? std::span<const uint32_t>(provoking_flat_fragment)
                                         : std::span<const uint32_t>(provoking_smooth_fragment);
    config.topology = c.shape == Shape::strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.width = size;
    config.height = size;
    config.layers = c.shape == Shape::layered ? 3 : 1;
    config.vertex_count = vertex_count(c.shape);
    config.provoking_vertex = c.provoking;
    config.dynamic_raster = c.dynamic;
    if (c.indexed)
        config.indices = std::span<const uint32_t>(indices).first(config.vertex_count);
    return context.draw(config);
}

std::array<uint8_t, 4> pixel(const DrawResult &result, unsigned layer, unsigned x, unsigned y)
{
    const size_t index = (size_t(layer) * size + y) * size + x;
    require(index < result.pixels.size(), "pixel outside the result");
    return result.pixels[index];
}

void near(uint8_t got, double want, double tolerance, const char *what, unsigned primitive)
{
    if (std::abs(double(got) - want) > tolerance)
        throw std::runtime_error(std::string(what) + " of primitive " + std::to_string(primitive) +
                                 ": got " + std::to_string(got) + ", want " + std::to_string(want));
}

// A flat tag t comes out as t / 16 in the red channel.
void check_flat(const DrawResult &result, const Case &c)
{
    const auto list = primitives(c.shape);
    for (unsigned n = 0; n < list.size(); ++n) {
        const uint32_t tag = c.provoking == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                 ? list[n].vertices[2]
                                 : list[n].vertices[0];
        const auto got = pixel(result, 0, list[n].x, list[n].y);
        near(got[0], tag * 255.0 / 16.0, 1.0, "flat tag", n);
        near(got[3], 255, 0, "coverage", n);
    }
}

struct Point {
    double x;
    double y;
};

// Where provoking_unroll.vert places vertex k, in target pixels.
Point position(Shape shape, uint32_t k)
{
    if (shape == Shape::strip)
        return {4.0 + 8.0 * k, (k % 2) == 0 ? 8.0 : 56.0};
    const uint32_t cell = k / 3;
    const double x = (cell % 2) * 32.0;
    const double y = (cell / 2) * 32.0;
    const uint32_t corner = k % 3;
    return corner == 0 ? Point{x + 4.0, y + 4.0}
                       : corner == 1 ? Point{x + 28.0, y + 4.0} : Point{x + 4.0, y + 28.0};
}

// The value is (k / 16, (k % 3) / 3, 0.5) at vertex k, interpolated linearly
// to the centre of the pixel.
void check_smooth(const DrawResult &result, const Case &c)
{
    const auto list = primitives(c.shape);
    for (unsigned n = 0; n < list.size(); ++n) {
        const Point a = position(c.shape, list[n].vertices[0]);
        const Point b = position(c.shape, list[n].vertices[1]);
        const Point d = position(c.shape, list[n].vertices[2]);
        const double px = list[n].x + 0.5;
        const double py = list[n].y + 0.5;
        const double area = (b.x - a.x) * (d.y - a.y) - (d.x - a.x) * (b.y - a.y);
        const double wb = ((px - a.x) * (d.y - a.y) - (d.x - a.x) * (py - a.y)) / area;
        const double wd = ((b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y)) / area;
        const std::array<double, 3> weight{1.0 - wb - wd, wb, wd};
        double red = 0;
        double green = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const uint32_t k = list[n].vertices[i];
            red += weight[i] * (k / 16.0);
            green += weight[i] * ((k % 3) / 3.0);
        }
        const auto got = pixel(result, 0, list[n].x, list[n].y);
        near(got[0], red * 255.0, 2.0, "red", n);
        near(got[1], green * 255.0, 2.0, "green", n);
        near(got[2], 0.5 * 255.0, 2.0, "blue", n);
        near(got[3], 255, 0, "coverage", n);
    }
}

// The triangle lands in layer 0 under the first-vertex convention and in
// layer 2 under the last-vertex one, whatever the other layers hold.
void check_layer(const DrawResult &result, const Case &c)
{
    const unsigned expected = c.provoking == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ? 2 : 0;
    for (unsigned layer = 0; layer < 3; ++layer) {
        const auto got = pixel(result, layer, 12, 12);
        near(got[3], layer == expected ? 255 : 0, 0, "layer coverage", layer);
    }
}

const char *name(const Case &c)
{
    static std::string text;
    text = std::string(c.shape == Shape::list ? "list" : c.shape == Shape::strip ? "strip" : "layered") +
           (c.provoking == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ? " last" : " first") +
           (c.read_flat ? " flat" : " smooth") + (c.dynamic ? " dynamic" : "") +
           (c.indexed ? " indexed" : "");
    return text.c_str();
}
} // namespace

int main()
{
    StderrCapture capture;
    // The driver reads its debug flags when the instance is created.
    setenv("MESA_KK_DEBUG", "unroll", 1);
    std::string current;
    try {
        DrawContext context;
        require_venus_when_asked(context);
        const bool kosmickrisp = context.capabilities().driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP;
        (void)capture.take();

        constexpr auto first = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        constexpr auto last = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
        unsigned cases = 0;
        unsigned unrolls = 0;
        for (const Shape shape : {Shape::list, Shape::strip}) {
            for (const bool indexed : {false, true}) {
                if (indexed && shape != Shape::list)
                    continue;
                // The first-vertex draws are the reference: no rewrite, no unroll.
                Case flat_first{shape, first, true, false, indexed};
                Case flat_last{shape, last, true, false, indexed};
                Case smooth_first{shape, first, false, false, indexed};
                Case smooth_last{shape, last, false, false, indexed};
                Case dynamic_flat{shape, last, true, true, indexed};
                Case dynamic_smooth{shape, last, false, true, indexed};

                const auto drawn = [&](const Case &c, bool expect_rewrite) {
                    current = name(c);
                    std::printf("Provoking unroll %s\n", current.c_str());
                    std::fflush(stdout);
                    const DrawResult result = run(context, c);
                    const Counts counts = count(capture.take());
                    ++cases;
                    if (kosmickrisp) {
                        require(counts.draws == 1, "the draw was not reported once");
                        require(counts.rewrites == (expect_rewrite ? 1u : 0u),
                                expect_rewrite ? "the provoking vertex was not rewritten"
                                               : "the provoking vertex was rewritten for nothing");
                        // A rewrite is an unroll; nothing else unrolls these draws.
                        require(counts.unrolled == counts.rewrites,
                                expect_rewrite ? "the rewrite did not unroll the draw"
                                               : "the draw was unrolled for nothing");
                        unrolls += counts.unrolled;
                    }
                    return result;
                };

                check_flat(drawn(flat_first, false), flat_first);
                check_flat(drawn(flat_last, true), flat_last);
                check_flat(drawn(dynamic_flat, true), dynamic_flat);

                const DrawResult reference = drawn(smooth_first, false);
                check_smooth(reference, smooth_first);
                const DrawResult smooth = drawn(smooth_last, false);
                check_smooth(smooth, smooth_last);
                require(smooth.pixels == reference.pixels,
                        "the provoking vertex changed an interpolated result");
                const DrawResult dynamic = drawn(dynamic_smooth, false);
                require(dynamic.pixels == reference.pixels,
                        "a dynamic provoking vertex changed an interpolated result");
            }
        }

        // The layer comes from the provoking vertex, so the draw that writes it
        // keeps its rewrite.
        for (const auto provoking : {first, last}) {
            const Case layered{Shape::layered, provoking, false};
            current = name(layered);
            std::printf("Provoking unroll %s\n", current.c_str());
            std::fflush(stdout);
            const DrawResult result = run(context, layered);
            const Counts counts = count(capture.take());
            check_layer(result, layered);
            ++cases;
            if (kosmickrisp) {
                const unsigned expected = provoking == last ? 1u : 0u;
                require(counts.draws == 1, "the layered draw was not reported once");
                require(counts.rewrites == expected && counts.unrolled == expected,
                        "the layered draw's rewrite does not follow the provoking vertex");
                unrolls += counts.unrolled;
            }
        }

        std::printf("Provoking unroll: %u cases passed, %u unrolled%s\n", cases, unrolls,
                    kosmickrisp ? "" : " (counts not checked on this driver)");
    } catch (const std::exception &error) {
        const std::string log = capture.take();
        std::fprintf(stdout, "Provoking unroll failed %s: %s\n%s", current.c_str(), error.what(),
                     log.c_str());
        return 1;
    }
    return 0;
}
