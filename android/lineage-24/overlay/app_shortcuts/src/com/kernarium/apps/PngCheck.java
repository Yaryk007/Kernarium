// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.kernarium.apps;

import com.kernarium.icons.IconProtocol;

import java.util.zip.CRC32;

/**
 * What the tool checks of an icon the renderer answered, because the renderer is not trusted (it draws what
 * applications' own files say): the size is one of the sizes asked for, the PNG is within the bound, starts with the
 * signature and a valid IHDR chunk whose dimensions are the size and whose format is 8-bit RGB or RGBA, not
 * interlaced. The rest of the stream is the picture, which the host decodes.
 */
final class PngCheck {
    /** The signature and the IHDR chunk (length, type, 13 bytes of data, CRC): the least a PNG can be checked by. */
    static final int HEADER_BYTES = 33;

    private PngCheck() {}

    /** Why the icon is not acceptable, or null when it is. */
    static String problem(Icon icon) {
        if (icon == null) {
            return "no icon";
        }
        boolean sized = false;
        for (int size : IconProtocol.SIZES) {
            sized |= icon.size == size;
        }
        if (!sized) {
            return "a size that was not asked for";
        }
        byte[] png = icon.png;
        if (png == null || png.length < HEADER_BYTES || png.length > IconProtocol.MAX_PNG_BYTES) {
            return "a PNG of a length that is not allowed";
        }
        for (int i = 0; i < AppsCommand.PNG_SIGNATURE.length; i++) {
            if (png[i] != AppsCommand.PNG_SIGNATURE[i]) {
                return "no PNG signature";
            }
        }
        if (integer(png, 8) != 13 || integer(png, 12) != 0x49484452) {
            return "no IHDR chunk first";
        }
        CRC32 crc = new CRC32();
        crc.update(png, 12, 17);
        if (crc.getValue() != (integer(png, 29) & 0xFFFFFFFFL)) {
            return "a damaged IHDR chunk";
        }
        if (integer(png, 16) != icon.size || integer(png, 20) != icon.size) {
            return "dimensions that are not the size";
        }
        int depth = png[24] & 0xFF;
        int colour = png[25] & 0xFF;
        if (depth != 8 || (colour != 2 && colour != 6) || png[26] != 0 || png[27] != 0 || png[28] != 0) {
            return "a format that is not 8-bit RGB or RGBA, not interlaced";
        }
        if (colour == 2 && icon.hasAlpha) {
            return "an alpha claim for a picture without an alpha channel";
        }
        return null;
    }

    private static int integer(byte[] data, int at) {
        return ((data[at] & 0xFF) << 24) | ((data[at + 1] & 0xFF) << 16) | ((data[at + 2] & 0xFF) << 8)
                | (data[at + 3] & 0xFF);
    }
}
