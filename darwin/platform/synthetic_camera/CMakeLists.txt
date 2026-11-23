# SPDX-License-Identifier: GPL-2.0-or-later

# The camera with no hardware (darwin/platform/camera.hpp), for tests and for the
# launchers that run a guest against a chart.
add_library(uml_synthetic_camera STATIC
    synthetic_camera.cpp
    synthetic_jpeg.mm)
add_library(UML::SyntheticCamera ALIAS uml_synthetic_camera)
uml_declare_architecture_target(uml_synthetic_camera platform)
target_include_directories(uml_synthetic_camera PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(uml_synthetic_camera PUBLIC UML::Nv12Transform)
target_link_libraries(uml_synthetic_camera PRIVATE
    UML::PhotoDimensions
    UML::PlatformContract
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework Foundation"
    "-framework ImageIO"
    "-framework UniformTypeIdentifiers")
