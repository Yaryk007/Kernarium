#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the transform feedback pass-chain regression shaders using an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    shaders = [
        Shader("feedback_step_vertex", "transform_feedback_passes.vert"),
        Shader("feedback_raster_vertex", "transform_feedback_passes_raster.vert"),
        Shader("feedback_raster_fragment", "transform_feedback_passes.frag"),
    ]
    generate("generate_transform_feedback_passes.py", __doc__,
             "transform_feedback_passes_spirv.hpp", shaders)


if __name__ == "__main__":
    main()
