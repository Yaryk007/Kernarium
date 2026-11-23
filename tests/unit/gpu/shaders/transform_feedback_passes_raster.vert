// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
// One point per vertex across the row below the middle of a 16 by 16 target.
void main()
{
    float x = (float(gl_VertexIndex) * 2.0 + 1.0) / 16.0 - 1.0;
    gl_Position = vec4(x, 1.0 / 16.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}
