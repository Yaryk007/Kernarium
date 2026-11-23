// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
// One step of a simulation that feeds itself: what a step captures is the
// next step's vertex input. The arithmetic is exact in 32-bit integers, so the
// whole chain has one right answer.
layout(location = 0) in uvec4 previous;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out uvec4 next;
void main()
{
    next = previous * 3u + uvec4(1u, 2u, 3u, 4u);
    // The point the rasterizing stage draws for this vertex, so that a step
    // drawn with the rasterizer on paints the same pixels.
    float x = (float(gl_VertexIndex) * 2.0 + 1.0) / 16.0 - 1.0;
    gl_Position = vec4(x, 1.0 / 16.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}
