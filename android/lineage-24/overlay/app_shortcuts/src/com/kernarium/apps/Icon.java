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

/** A rendered icon: a PNG of {@code size} by {@code size} pixels. */
final class Icon {
    final int size;
    /** Composed from an adaptive icon's layers (no mask), else a single drawable scaled to the square. */
    final boolean adaptive;
    /** Whether any pixel is not fully opaque. */
    final boolean hasAlpha;
    final byte[] png;

    Icon(int size, boolean adaptive, boolean hasAlpha, byte[] png) {
        this.size = size;
        this.adaptive = adaptive;
        this.hasAlpha = hasAlpha;
        this.png = png;
    }
}
