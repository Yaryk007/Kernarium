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

import java.io.BufferedOutputStream;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

/** The entry point app_process starts (see the wrapper script {@code kernarium_apps}). */
public final class Main {
    private Main() {}

    public static void main(String[] args) {
        // The answer may carry binary data (PNG), so it goes to the descriptor as bytes, not through System.out's encoder.
        BufferedOutputStream out = new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16);
        PrintWriter err = new PrintWriter(new OutputStreamWriter(System.err, StandardCharsets.UTF_8));
        int status;
        try {
            status = AppsCommand.run(args, AndroidPlatform::new, out, err);
        } catch (Throwable problem) {
            err.print(AppsCommand.NAME + ": " + AppsCommand.reason(problem) + "\n");
            status = AppsCommand.EXIT_FAILURE;
        }
        try {
            out.flush();
        } catch (java.io.IOException problem) {
            status = AppsCommand.EXIT_FAILURE;
        }
        err.flush();
        System.exit(status);
    }
}
