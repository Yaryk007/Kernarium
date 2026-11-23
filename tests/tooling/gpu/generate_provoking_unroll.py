#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the provoking vertex unroll regression shaders using an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    shaders = [
        Shader("provoking_list_vertex", "provoking_unroll.vert", ["-DSTRIP=0", "-DLAYER=0"]),
        Shader("provoking_strip_vertex", "provoking_unroll.vert", ["-DSTRIP=1", "-DLAYER=0"]),
        Shader("provoking_layer_vertex", "provoking_unroll.vert", ["-DSTRIP=0", "-DLAYER=1"]),
        Shader("provoking_flat_fragment", "provoking_unroll.frag", ["-DREAD_FLAT=1"]),
        Shader("provoking_smooth_fragment", "provoking_unroll.frag", ["-DREAD_FLAT=0"]),
    ]
    generate("generate_provoking_unroll.py", __doc__, "provoking_unroll_spirv.hpp", shaders)


if __name__ == "__main__":
    main()
