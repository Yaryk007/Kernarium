// SPDX-License-Identifier: GPL-2.0-or-later
#include "synthetic_camera.hpp"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>

namespace uml::platform {

std::vector<std::uint8_t> encodeSyntheticJpeg(const std::uint8_t* luma, std::size_t lumaStride, const std::uint8_t* chroma,
                                              std::size_t chromaStride, std::uint32_t width, std::uint32_t height,
                                              unsigned quality)
{
    if (width == 0 || height == 0 || (width & 1) || (height & 1)) return {};
    // BT.709 full range to 8-bit RGB.
    std::vector<std::uint8_t> rgba(std::size_t(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const double yy = luma[std::size_t(y) * lumaStride + x];
            const auto* uv = chroma + std::size_t(y / 2) * chromaStride + (x & ~1u);
            const double cb = uv[0] - 128.0;
            const double cr = uv[1] - 128.0;
            const auto r = std::clamp(yy + 1.5748 * cr, 0.0, 255.0);
            const auto g = std::clamp(yy - 0.1873 * cb - 0.4681 * cr, 0.0, 255.0);
            const auto b = std::clamp(yy + 1.8556 * cb, 0.0, 255.0);
            auto* out = &rgba[(std::size_t(y) * width + x) * 4];
            out[0] = std::uint8_t(r + 0.5);
            out[1] = std::uint8_t(g + 0.5);
            out[2] = std::uint8_t(b + 0.5);
            out[3] = 255;
        }
    }
    std::vector<std::uint8_t> result;
    @autoreleasepool {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(rgba.data(), width, height, 8, std::size_t(width) * 4, space,
                                                     CGBitmapInfo(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrderDefault);
        CGColorSpaceRelease(space);
        if (context == nullptr) return {};
        CGImageRef image = CGBitmapContextCreateImage(context);
        CGContextRelease(context);
        if (image == nullptr) return {};
        NSMutableData* data = [NSMutableData data];
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
        if (destination != nullptr) {
            // The make in the TIFF dictionary makes ImageIO write an Exif segment.
            NSDictionary* properties = @{
                (__bridge NSString*)kCGImageDestinationLossyCompressionQuality: @(std::clamp(quality, 1u, 100u) / 100.0),
                (__bridge NSString*)kCGImagePropertyTIFFDictionary: @{(__bridge NSString*)kCGImagePropertyTIFFMake: @"Synthetic"},
            };
            CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)properties);
            if (CGImageDestinationFinalize(destination)) {
                const auto* bytes = static_cast<const std::uint8_t*>(data.bytes);
                result.assign(bytes, bytes + data.length);
            }
            CFRelease(destination);
        }
        CGImageRelease(image);
    }
    return result;
}

} // namespace uml::platform
