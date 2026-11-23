// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#if LAYER
#extension GL_ARB_shader_viewport_layer_array : require
#endif
// The flat output is always written; whether anything reads it is the
// fragment shader's choice.
layout(location = 1) flat out uint tag;
layout(location = 2) out vec3 value;
void main()
{
    uint k = uint(gl_VertexIndex);
#if STRIP
    // A zigzag across the target: primitive n is vertices n, n + 1, n + 2.
    vec2 p = vec2(4.0 + 8.0 * float(k), (k % 2u) == 0u ? 8.0 : 56.0);
#else
    // Triangle t sits in cell t of a two by two grid.
    uint t = k / 3u;
    uint j = k % 3u;
    vec2 cell = vec2(float(t % 2u), float(t / 2u)) * 32.0;
    vec2 p = cell + (j == 0u ? vec2(4.0, 4.0)
                             : j == 1u ? vec2(28.0, 4.0) : vec2(4.0, 28.0));
#endif
    gl_Position = vec4(p / 32.0 - 1.0, 0.0, 1.0);
    tag = k;
    value = vec3(float(k) / 16.0, float(k % 3u) / 3.0, 0.5);
#if LAYER
    // The first vertex of a triangle selects layer 0, its last vertex layer 2.
    gl_Layer = int(k % 3u);
#endif
}
