// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) out vec4 color;
#if READ_FLAT
layout(location = 1) flat in uint tag;
#else
layout(location = 2) in vec3 value;
#endif
void main()
{
#if READ_FLAT
    color = vec4(float(tag) / 16.0, 0.0, 0.0, 1.0);
#else
    color = vec4(value, 1.0);
#endif
}
