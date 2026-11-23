// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The front camera's capture formats as a platform lists them, for the tests of
// the front still limit (photo_dimensions.hpp, limitedStillFormats).

#include "camera.hpp"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace uml::testing {

// Rows of "<width>x<height> <min>-<max> [m] [f] {<photo sizes>}": the format's size
// and one frame-rate span, `m` when a multi-camera session takes the format, `f`
// when it is full range, and the photo sizes its output lists.
inline std::vector<uml::platform::CameraCaptureFormat> parseFormatRows(const std::string& rows)
{
    std::vector<uml::platform::CameraCaptureFormat> formats;
    std::istringstream lines(rows);
    for (std::string line; std::getline(lines, line);) {
        const auto open = line.find('{');
        const auto close = line.find('}');
        if (open == std::string::npos || close == std::string::npos) continue;
        uml::platform::CameraCaptureFormat format;
        std::istringstream head(line.substr(0, open));
        std::string token;
        while (head >> token) {
            unsigned width = 0, height = 0;
            double low = 0, high = 0;
            if (format.width == 0 && std::sscanf(token.c_str(), "%ux%u", &width, &height) == 2) {
                format.width = width;
                format.height = height;
            } else if (std::sscanf(token.c_str(), "%lf-%lf", &low, &high) == 2) {
                format.frameRates.push_back({low, high});
            } else if (token == "m") {
                format.multiCamera = true;
            } else if (token == "f") {
                format.fullRange = true;
            }
        }
        std::istringstream sizes(line.substr(open + 1, close - open - 1));
        for (std::string size; sizes >> size;) {
            unsigned width = 0, height = 0;
            if (std::sscanf(size.c_str(), "%ux%u", &width, &height) == 2) format.photoSizes.push_back({width, height});
        }
        formats.push_back(std::move(format));
    }
    return formats;
}

// The front camera of an iPad mini 6 (its 12 MP ultra wide lens), as the Apple backend lists it: the 4:3 formats
// make the 3088x2316 photo (and 2016x1512), the 16:9 formats 2304x1296.
inline std::vector<uml::platform::CameraCaptureFormat> miniFrontFormats()
{
    return parseFormatRows(R"(192x144 1-60 {192x144 1440x1080}
192x144 1-60 f {192x144 1440x1080}
352x288 1-60 {352x288 1320x1080}
352x288 1-60 f {352x288 1320x1080}
480x360 1-60 {480x360 1440x1080}
480x360 1-60 f {480x360 1440x1080}
640x480 1-60 {640x480 1440x1080}
640x480 1-60 f {640x480 1440x1080}
640x480 1-60 m {640x480 1440x1080}
640x480 1-60 m f {640x480 1440x1080}
960x540 1-30 {960x540 2304x1296}
960x540 1-30 f {960x540 2304x1296}
1024x768 1-60 {1024x768 1440x1080}
1024x768 1-60 f {1024x768 1440x1080}
1280x720 1-30 {1280x720 2304x1296}
1280x720 1-30 f {1280x720 2304x1296}
1280x720 1-60 {1280x720 2304x1296}
1280x720 1-60 f {1280x720 2304x1296}
1280x720 1-60 m {1280x720 1408x792}
1280x720 1-60 m f {1280x720 1408x792}
1440x1080 1-60 m {1440x1080}
1440x1080 1-60 m f {1440x1080}
1920x1080 1-30 m {1920x1080 2304x1296}
1920x1080 1-30 m f {1920x1080 2304x1296}
1920x1080 1-60 m {1920x1080 2304x1296}
1920x1080 1-60 m f {1920x1080 2304x1296}
1920x1440 1-30 m {1920x1440 2016x1512}
1920x1440 1-30 m f {1920x1440 2016x1512}
1920x1440 1-60 {1920x1440 2016x1512}
1920x1440 1-60 f {1920x1440 2016x1512}
2016x1512 1-30 {2016x1512 3088x2316}
2016x1512 1-30 f {2016x1512 3088x2316}
)");
}

// The front camera of an iPhone 15 Pro Max (its 12 MP wide lens), as the Apple backend lists it: the second real
// front table. Its multi-camera formats make 4032x3024 (in 1920x1440) and 2016x1512, and it keeps its concurrency.
inline std::vector<uml::platform::CameraCaptureFormat> phoneFrontFormats()
{
    return parseFormatRows(R"(192x144 1-60 {192x144 4032x3024}
192x144 1-60 f {192x144 4032x3024}
352x288 1-60 {352x288 3696x3024}
352x288 1-60 f {352x288 3696x3024}
480x360 1-60 {480x360 4032x3024}
480x360 1-60 f {480x360 4032x3024}
640x480 1-60 {640x480 4032x3024}
640x480 1-60 f {640x480 4032x3024}
640x480 1-60 m {640x480 2016x1512}
640x480 1-60 m f {640x480 2016x1512}
960x540 1-30 {960x540 4032x2268}
960x540 1-30 f {960x540 4032x2268}
1024x768 1-60 {1024x768 4032x3024}
1024x768 1-60 f {1024x768 4032x3024}
1280x720 1-30 {1280x720 4032x2268}
1280x720 1-30 f {1280x720 4032x2268}
1280x720 1-60 {1280x720 4032x2268}
1280x720 1-60 f {1280x720 4032x2268}
1280x720 1-60 m {1280x720 2016x1136}
1280x720 1-60 m f {1280x720 2016x1136}
1280x720 1-120 {1280x720}
1280x720 1-120 f {1280x720}
1440x1080 1-60 m {1440x1080 2016x1512}
1440x1080 1-60 m f {1440x1080 2016x1512}
1920x1080 1-30 m {1920x1080 4032x2268}
1920x1080 1-30 m f {1920x1080 4032x2268}
1920x1080 1-60 m {1920x1080 4032x2268}
1920x1080 1-60 m f {1920x1080 4032x2268}
1920x1080 1-60 m {1920x1080 2016x1136}
1920x1080 1-60 m f {1920x1080 2016x1136}
1920x1080 1-120 {1920x1080}
1920x1080 1-120 f {1920x1080}
1920x1440 1-30 m {1920x1440 4032x3024}
1920x1440 1-30 m f {1920x1440 4032x3024}
1920x1440 1-60 m {1920x1440 2016x1512}
1920x1440 1-60 m f {1920x1440 2016x1512}
3088x2316 1-30 {3088x2316}
3088x2316 1-30 f {3088x2316}
3840x2160 1-30 m {3840x2160 4032x2268}
3840x2160 1-30 m f {3840x2160 4032x2268}
3840x2160 1-60 {3840x2160}
3840x2160 1-60 f {3840x2160}
4032x3024 1-30 {4032x3024}
4032x3024 1-30 f {4032x3024}
)");
}

} // namespace uml::testing
