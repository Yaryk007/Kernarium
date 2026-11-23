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

import android.app.ActivityManager;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.ParceledListSlice;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Build;
import android.os.LocaleList;
import android.os.ServiceManager;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.util.DisplayMetrics;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * {@link OldAppCommand.Platform} on Android, for a process that runs as the adb shell user: the
 * package manager through its binder interface (the owner user's applications), the label from
 * the application's own resources (the shell user may read any application's package), the
 * property through the system properties, and {@code cmd activity force-stop} for the stop.
 */
final class AndroidPlatform implements OldAppCommand.Platform {
    private static final int FORCE_STOP_SECONDS = 30;
    private static final int GETPROP_SECONDS = 30;
    /** The shell user's own data directory (shell_data_file); the command's lock lives in it. */
    private static final String LOCK_FILE = "/data/local/tmp/kernarium_oldapp.lock";
    private static final int OUTPUT_LIMIT = 400;

    private final IPackageManager mPackageManager;
    private final int mUserId = UserHandle.myUserId();
    private Configuration mConfiguration;

    AndroidPlatform() {
        mPackageManager = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (mPackageManager == null) {
            throw new IllegalStateException("the package manager is not running");
        }
    }

    @Override
    public List<Candidate> installedPackages() throws Exception {
        ParceledListSlice<PackageInfo> slice = mPackageManager.getInstalledPackages(0L, mUserId);
        List<Candidate> result = new ArrayList<>();
        for (PackageInfo info : slice.getList()) {
            Candidate candidate = candidate(info);
            if (candidate != null) {
                result.add(candidate);
            }
        }
        return result;
    }

    @Override
    public Candidate find(String packageName) throws Exception {
        PackageInfo info = mPackageManager.getPackageInfo(packageName, 0L, mUserId);
        return info == null ? null : candidate(info);
    }

    private static Candidate candidate(PackageInfo info) {
        ApplicationInfo application = info.applicationInfo;
        if (application == null) {
            return null;
        }
        return new Candidate(info.packageName, (application.flags & ApplicationInfo.FLAG_SYSTEM) != 0,
                application.primaryCpuAbi, application.targetSdkVersion, info.sharedUserId,
                info.firstInstallTime, info.lastUpdateTime / 1000, info);
    }

    @Override
    public List<String> abis32() {
        return Arrays.asList(Build.SUPPORTED_32_BIT_ABIS);
    }

    @Override
    public String label(Candidate candidate) {
        ApplicationInfo application = ((PackageInfo) candidate.source).applicationInfo;
        CharSequence text = application.nonLocalizedLabel;
        if (text == null && application.labelRes != 0) {
            text = resourceText(application);
        }
        // The launcher's order: the label, then the package name.
        return text == null ? candidate.packageName : text.toString().trim();
    }

    /** The label resource of the application's own package in the system's current configuration. */
    private CharSequence resourceText(ApplicationInfo application) {
        AssetManager assets = new AssetManager();
        try {
            if (application.sourceDir == null || assets.addAssetPath(application.sourceDir) == 0) {
                return null;
            }
            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            return new Resources(assets, metrics, configuration()).getText(application.labelRes, null);
        } finally {
            assets.close();
        }
    }

    /** The system's current configuration (its locales above all), or the process's default one. */
    private Configuration configuration() {
        if (mConfiguration == null) {
            Configuration configuration = null;
            try {
                configuration = ActivityManager.getService().getConfiguration();
            } catch (Exception problem) {
                // The default locales below are what a process of this uid has.
            }
            if (configuration == null) {
                configuration = new Configuration();
                configuration.setLocales(LocaleList.getDefault());
            }
            mConfiguration = configuration;
        }
        return mConfiguration;
    }

    @Override
    public String getProperty(String name) {
        return SystemProperties.get(name, "");
    }

    @Override
    public void setProperty(String name, String value) {
        SystemProperties.set(name, value);
    }

    /**
     * The packages with a stored property, named by the output of {@code getprop} (the system
     * properties have no list call); {@link PropertyListing} reads that output, whose values the
     * command does not take from it.
     */
    @Override
    public Collection<String> storedPackages() throws Exception {
        Process process = new ProcessBuilder("/system/bin/getprop").redirectErrorStream(true).start();
        process.getOutputStream().close();
        Collection<String> packages;
        try (BufferedReader lines = new BufferedReader(
                new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
            packages = PropertyListing.storedPackages(lines);
        }
        if (!process.waitFor(GETPROP_SECONDS, TimeUnit.SECONDS) || process.exitValue() != 0) {
            process.destroy();
            throw new IllegalStateException("getprop did not finish cleanly");
        }
        return packages;
    }

    /** {@link LockFile} on the shell user's own data directory. */
    @Override
    public OldAppCommand.Lock lock(long timeoutMillis) throws Exception {
        return LockFile.acquire(Paths.get(LOCK_FILE), timeoutMillis);
    }

    @Override
    public void forceStop(String packageName) throws Exception {
        Process process = new ProcessBuilder("/system/bin/cmd", "activity", "force-stop", packageName)
                .redirectErrorStream(true).start();
        process.getOutputStream().close();
        // The command prints next to nothing (the pipe holds far more), so the wait comes first and has the bound.
        if (!process.waitFor(FORCE_STOP_SECONDS, TimeUnit.SECONDS)) {
            process.destroy();
            throw new IllegalStateException("cmd activity force-stop did not finish");
        }
        if (process.exitValue() != 0) {
            StringBuilder output = new StringBuilder();
            try (InputStream in = process.getInputStream()) {
                byte[] buffer = new byte[OUTPUT_LIMIT];
                int n = in.read(buffer);
                if (n > 0) {
                    output.append(new String(buffer, 0, n, "UTF-8"));
                }
            }
            String text = output.toString().trim();
            throw new IllegalStateException("cmd activity force-stop exited " + process.exitValue()
                    + (text.isEmpty() ? "" : ": " + text));
        }
    }

    @Override
    public long nowSeconds() {
        return System.currentTimeMillis() / 1000;
    }
}
