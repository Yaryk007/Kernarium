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

package com.kernarium.oldapp;

import java.io.BufferedReader;
import java.io.IOException;
import java.util.Set;
import java.util.TreeSet;

/**
 * Which packages have a stored choice, from the output of {@code getprop} (the system properties
 * have no list call).
 *
 * <p>The output has one {@code [name]: [value]} per property, but a value may itself hold line
 * breaks, so a line is never taken to be a whole property and a value is never taken from it:
 * the listing only names the properties, whose values the command then reads by name. A line
 * that starts like a property inside a value names a property that is not there, which reads as
 * empty.
 */
final class PropertyListing {
    private PropertyListing() {}

    /** The package part of every property under the store's prefix whose name can be a key. */
    static Set<String> storedPackages(BufferedReader lines) throws IOException {
        Set<String> packages = new TreeSet<>();
        String line;
        while ((line = lines.readLine()) != null) {
            if (!line.startsWith("[")) {
                continue;
            }
            int end = line.indexOf("]: [");
            if (end < 0) {
                continue;
            }
            String key = line.substring(1, end);
            if (key.startsWith(OldAppEligibility.STORE_PREFIX)) {
                String packageName = key.substring(OldAppEligibility.STORE_PREFIX.length());
                if (OldAppEligibility.isPropertyKeyPart(packageName)) {
                    packages.add(packageName);
                }
            }
        }
        return packages;
    }
}
