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

/**
 * A failure whose reason is a fixed text of this tool. It is the only kind of message the command prints
 * for a failure: the text of any other exception (a framework's can carry a user id, a permission name
 * or a path) never reaches the output, only its class's name does (see {@link AppsCommand#reason}).
 */
final class PlatformException extends Exception {
    private static final long serialVersionUID = 1L;

    PlatformException(String reason) {
        super(reason);
    }

    PlatformException(String reason, Throwable cause) {
        super(reason, cause);
    }
}
