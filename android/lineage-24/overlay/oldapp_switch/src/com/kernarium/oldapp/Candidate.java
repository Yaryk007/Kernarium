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

/**
 * What the eligibility rule needs to know about an installed application, free of any platform
 * class so that the rule and the command around it run on a plain JVM.
 */
final class Candidate {
    final String packageName;
    /** FLAG_SYSTEM: an updated system application keeps it. */
    final boolean system;
    /** The primary ABI the package manager chose, or null for an application without native code. */
    final String primaryCpuAbi;
    final int targetSdkVersion;
    /** The android:sharedUserId, or null. */
    final String sharedUserId;
    /**
     * The first install, in epoch milliseconds as the package manager reports it. It names the
     * installation: an update keeps it, an uninstall and a new install make a new one.
     */
    final long firstInstallMillis;
    /** The last install or update, in epoch seconds. */
    final long lastUpdateSeconds;
    /** The platform's own record of the application, for the platform to resolve the label from. */
    final Object source;

    Candidate(String packageName, boolean system, String primaryCpuAbi, int targetSdkVersion,
            String sharedUserId, long firstInstallMillis, long lastUpdateSeconds, Object source) {
        this.packageName = packageName;
        this.system = system;
        this.primaryCpuAbi = primaryCpuAbi;
        this.targetSdkVersion = targetSdkVersion;
        this.sharedUserId = sharedUserId;
        this.firstInstallMillis = firstInstallMillis;
        this.lastUpdateSeconds = lastUpdateSeconds;
        this.source = source;
    }
}
