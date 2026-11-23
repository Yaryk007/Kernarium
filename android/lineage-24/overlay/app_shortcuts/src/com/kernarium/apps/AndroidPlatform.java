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

import android.app.ActivityManager;
import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.DeadObjectException;
import android.os.IBinder;
import android.os.LocaleList;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.ArraySet;
import android.util.DisplayMetrics;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import com.kernarium.icons.IconProtocol;

/**
 * {@link AppsCommand.Platform} on Android, for a process that runs as the adb shell user: the package
 * manager through its binder interface (the foreground user's applications), the labels from each
 * application's own resources (the shell user may read any application's package; that reads string
 * tables only, no image or XML file), the icons from the renderer app (KernariumIcons: no icon is drawn
 * in this process), and {@code cmd activity start-activity} for the start. Nothing is written.
 *
 * <p>No question has a substitute answer: when the foreground user, a permission, the system's
 * configuration or a package's facts cannot be had, the command fails (status 1); a package that is gone
 * between the query and its facts is left out.
 */
final class AndroidPlatform implements AppsCommand.Platform {
    private static final int OUTPUT_LIMIT = 4096;
    /** The density labels are read for: the largest, with day mode and no other qualifier (see fixedConfiguration). */
    private static final int LABEL_DENSITY = DisplayMetrics.DENSITY_XXXHIGH;
    /** The system assets the framework builds (AssetManager.FRAMEWORK_APK_PATH and LINEAGE_APK_PATH). */
    private static final String FRAMEWORK_APK = "/system/framework/framework-res.apk";
    private static final String PLATFORM_APK = "/system/framework/org.lineageos.platform-res.apk";
    private static boolean sSystemAssetsBuilt;

    private final IPackageManager mPackageManager;
    private final Map<String, PackageInfo> mPackages = new HashMap<>();
    private final Map<String, Boolean> mPermissions = new HashMap<>();
    private final Map<String, Resources> mLabelResources = new HashMap<>();
    private Configuration mConfiguration;

    AndroidPlatform() throws PlatformException {
        mPackageManager = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (mPackageManager == null) {
            throw new PlatformException("the package manager is not running");
        }
    }

    @Override
    public int currentUser() throws PlatformException {
        try {
            return ActivityManager.getCurrentUser();
        } catch (RuntimeException problem) {
            // No substitute (not the user this process runs for): a start or a listing for the wrong
            // user is worse than none.
            throw new PlatformException("the foreground user is not known", problem);
        }
    }

    @Override
    public List<Entry> launcherActivities(int user) throws Exception {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        long flags = PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        ParceledListSlice<ResolveInfo> slice = mPackageManager.queryIntentActivities(intent, null, flags, user);
        if (slice == null) {
            throw new PlatformException("the package manager gave no answer");
        }
        List<Entry> result = new ArrayList<>();
        for (ResolveInfo resolved : slice.getList()) {
            ActivityInfo info = resolved.activityInfo;
            if (info == null || info.applicationInfo == null || info.packageName == null || info.name == null) {
                continue;
            }
            PackageInfo packageInfo = packageInfo(info.packageName, user);
            if (packageInfo == null) {
                continue; // removed since the query
            }
            ApplicationInfo application = info.applicationInfo;
            List<ResourcePaths.Item> paths = resourcePaths(application);
            // Enabled: the resolver's own answer. The query does not ask for disabled components, so every
            // activity it returns is enabled for the user, and ActivityInfo.isEnabled() (the manifest's flag
            // and the application's) would drop an alias that is disabled in the manifest and enabled since.
            boolean builtIn = (application.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            // A built-in application that was never updated has an update time that is its APK's file time (one
            // fixed date in every image) or the wall time of the first boot that scanned it, and the resource
            // fingerprint names paths and not content: the image's own build time is an input of its own.
            long imageBuild = builtIn && (application.flags & ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0
                    ? Build.TIME : 0;
            result.add(new Entry(info.packageName, info.name, builtIn,
                    info.exported, true, permitted(info.permission), packageInfo.getLongVersionCode(),
                    packageInfo.lastUpdateTime, imageBuild, info.getIconResource(), ResourcePaths.fingerprint(paths),
                    info));
        }
        return result;
    }

    /** The package's facts for the user, or null when it is not installed for them (cached, absence too). */
    private PackageInfo packageInfo(String packageName, int user) throws Exception {
        if (!mPackages.containsKey(packageName)) {
            mPackages.put(packageName, mPackageManager.getPackageInfo(packageName, 0L, user));
        }
        return mPackages.get(packageName);
    }

    /** Whether the shell user holds the permission an activity asks for (none asked is permitted). */
    private boolean permitted(String permission) throws PlatformException {
        if (permission == null) {
            return true;
        }
        Boolean known = mPermissions.get(permission);
        if (known == null) {
            try {
                known = ActivityManager.getService().checkPermission(permission, android.os.Process.myPid(),
                        android.os.Process.myUid()) == PackageManager.PERMISSION_GRANTED;
            } catch (Exception problem) {
                throw new PlatformException("a permission could not be checked", problem);
            }
            mPermissions.put(permission, known);
        }
        return known;
    }

    @Override
    public List<String> localeTags() throws PlatformException {
        String joined = configuration().getLocales().toLanguageTags();
        return new ArrayList<>(Arrays.asList(joined.split(",")));
    }

    @Override
    public String label(Entry entry) throws Exception {
        ActivityInfo info = (ActivityInfo) entry.source;
        ApplicationInfo application = info.applicationInfo;
        // The launcher's order: the activity's label, then the application's.
        CharSequence text = text(info.nonLocalizedLabel, info.labelRes, application);
        if (text == null) {
            text = text(application.nonLocalizedLabel, application.labelRes, application);
        }
        return text == null ? null : text.toString();
    }

    private CharSequence text(CharSequence literal, int resource, ApplicationInfo application) throws Exception {
        if (literal != null) {
            return literal;
        }
        if (resource == 0) {
            return null;
        }
        Resources resources = resources(application);
        return resources == null ? null : resources.getText(resource, null);
    }

    @Override
    public AppsCommand.Renderer openRenderer(int user) throws Exception {
        return new ProviderRenderer(user);
    }

    /** The files an application's resources come from, for the user the application info describes. */
    private static List<ResourcePaths.Item> resourcePaths(ApplicationInfo application) {
        return ResourcePaths.of(application.publicSourceDir, application.sourceDir, application.splitPublicSourceDirs,
                application.splitSourceDirs, application.sharedLibraryFiles, application.resourceDirs,
                application.overlayPaths);
    }

    /**
     * The application's own resources, every file its effective resource path names (the base, its splits, its
     * shared libraries and the overlays enabled for the user, as the framework adds them for the application
     * itself): in the fixed configuration with the system's locale list, for the label. One per application. Null
     * when the base or a split cannot be added (the label is then the package name); a shared library or an overlay
     * that cannot be added is skipped, as the framework does.
     */
    private Resources resources(ApplicationInfo application) throws PlatformException {
        Map<String, Resources> cache = mLabelResources;
        if (!cache.containsKey(application.packageName)) {
            List<ResourcePaths.Item> paths = resourcePaths(application);
            Resources resources = null;
            systemAssetsWithoutIdmaps();
            AssetManager assets = new AssetManager();
            boolean complete = ResourcePaths.hasBase(paths);
            for (int i = 0; complete && i < paths.size(); i++) {
                ResourcePaths.Item item = paths.get(i);
                int cookie;
                switch (item.kind) {
                    case LIBRARY:
                        cookie = assets.addAssetPathAsSharedLibrary(item.path);
                        break;
                    case OVERLAY:
                        cookie = assets.addOverlayPath(item.path);
                        break;
                    default:
                        cookie = assets.addAssetPath(item.path);
                        break;
                }
                complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY
                        || item.kind == ResourcePaths.Kind.LIBRARY;
            }
            if (complete) {
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                metrics.densityDpi = LABEL_DENSITY;
                metrics.density = LABEL_DENSITY / (float) DisplayMetrics.DENSITY_DEFAULT;
                resources = new Resources(assets, metrics, fixedConfiguration(configuration().getLocales()));
            } else {
                assets.close();
            }
            cache.put(application.packageName, resources);
        }
        return cache.get(application.packageName);
    }

    /**
     * Builds the framework's system assets (framework-res and the platform's own resources) before the first
     * AssetManager asks for them, without the static overlays that target the framework. A process the zygote did not
     * fork builds them on its first AssetManager and runs idmap2 to make those overlays' idmaps, which the shell domain
     * may not execute: a denial on every run. The overlays change the framework's own values, not an application's
     * label or icon. The assets and their order are the framework's. The manager is published last, once it holds its
     * assets; when anything fails on the way (an exception, a framework that is not as written here, an allocation
     * failure) the statics are put back as they were found and the framework's own initialisation runs at the first
     * AssetManager, denial and all. An allocation failure is also passed on, for the caller's own recovery, and the
     * next call tries again.
     */
    private static synchronized void systemAssetsWithoutIdmaps() {
        if (sSystemAssetsBuilt) {
            return;
        }
        sSystemAssetsBuilt = true;
        Field system = null;
        Field array = null;
        Field set = null;
        Object previousArray = null;
        Object previousSet = null;
        boolean touched = false;
        try {
            system = AssetManager.class.getDeclaredField("sSystem");
            array = AssetManager.class.getDeclaredField("sSystemApkAssets");
            set = AssetManager.class.getDeclaredField("sSystemApkAssetsSet");
            system.setAccessible(true);
            array.setAccessible(true);
            set.setAccessible(true);
            if (system.get(null) != null) {
                return; // already built
            }
            previousArray = array.get(null);
            previousSet = set.get(null);
            List<ApkAssets> assets = new ArrayList<>();
            assets.add(ApkAssets.loadFromPath(FRAMEWORK_APK, ApkAssets.PROPERTY_SYSTEM));
            if (new File(PLATFORM_APK).exists()) {
                assets.add(ApkAssets.loadFromPath(PLATFORM_APK, ApkAssets.PROPERTY_SYSTEM));
            }
            ApkAssets[] all = assets.toArray(new ApkAssets[0]);
            Constructor<AssetManager> constructor = AssetManager.class.getDeclaredConstructor(boolean.class);
            constructor.setAccessible(true);
            // setApkAssets reads the two statics, so they are set first; the manager is published after it has its assets.
            touched = true;
            set.set(null, new ArraySet<ApkAssets>(assets));
            array.set(null, all);
            AssetManager manager = constructor.newInstance(true);
            manager.setApkAssets(all, false);
            system.set(null, manager);
        } catch (Exception | LinkageError problem) {
            restoreSystemAssets(touched, array, previousArray, set, previousSet);
        } catch (Error problem) {
            restoreSystemAssets(touched, array, previousArray, set, previousSet);
            sSystemAssetsBuilt = false;
            throw problem;
        }
    }

    /** Puts the two statics the bootstrap changed back as they were found (nothing is changed before {@code touched}). */
    private static void restoreSystemAssets(boolean touched, Field array, Object previousArray, Field set,
            Object previousSet) {
        if (!touched) {
            return;
        }
        try {
            set.set(null, previousSet);
            array.set(null, previousArray);
        } catch (Exception | LinkageError ignored) {
            // Nothing more can be done: the framework builds the assets over again.
        }
    }

    /**
     * What a label is read for: the highest density, day mode, the given locales, and no other qualifier (screen
     * size, orientation...), whatever the device's own state. A label then depends on the system's locale list
     * alone, which the digest covers, so the digest needs no configuration in it.
     */
    private static Configuration fixedConfiguration(LocaleList locales) {
        Configuration configuration = new Configuration();
        configuration.densityDpi = LABEL_DENSITY;
        configuration.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        configuration.setLocales(locales);
        return configuration;
    }

    /** The system's current configuration (its locales above all); there is no default to stand in for it. */
    private Configuration configuration() throws PlatformException {
        if (mConfiguration == null) {
            Configuration configuration;
            try {
                configuration = ActivityManager.getService().getConfiguration();
            } catch (Exception problem) {
                throw new PlatformException("the system's configuration cannot be read", problem);
            }
            if (configuration == null) {
                throw new PlatformException("the system's configuration cannot be read");
            }
            mConfiguration = configuration;
        }
        return mConfiguration;
    }

    @Override
    public AppsCommand.StartResult start(int user, Component component) throws Exception {
        ChildProcess.Result result = ChildProcess.run(AppsCommand.startArguments(user, component),
                AppsCommand.START_SECONDS * 1000L, OUTPUT_LIMIT, new KillByPid());
        return new AppsCommand.StartResult(result.exit, result.output);
    }

    @Override
    public long nowMillis() {
        return SystemClock.elapsedRealtime();
    }

    /**
     * SIGKILL for a child that SIGTERM did not end. This libcore's {@code Process.destroyForcibly()} is
     * {@code destroy()} (SIGTERM) again and the class has no {@code pid()}, so the pid is read from the child's
     * own field and {@code android.os.Process.killProcess} sends the signal. This libcore has no handle bound to the
     * child, so the child must be alive right before the signal (a reaped child's number may be another's); the
     * reaper running between that check and the signal is the one window left, as it is for the JDK's own destroy. A
     * failure here is not reported: the wait that follows says whether the child is gone.
     */
    private static final class KillByPid implements ChildProcess.Kill {
        @Override
        public void kill(Process process) throws Exception {
            if (!process.isAlive()) {
                return; // a number that was the child's may be another process's now
            }
            java.lang.reflect.Field field = process.getClass().getDeclaredField("pid");
            field.setAccessible(true);
            android.os.Process.killProcess(field.getInt(process));
        }
    }

    /**
     * The icon renderer (KernariumIcons) as the adb shell user reaches it: its content provider, held through the
     * activity manager the way the {@code content} command holds one (the provider is open to the shell user alone),
     * for the user the command listed the applications for. The provider hands each icon to a service in an isolated
     * process, which opens the application's files and draws; this process only names the files and receives a PNG,
     * which the command checks. {@link ProviderLink} holds the rules: whose provider it is, the one user, a provider
     * that died, and a hold that stays closed.
     */
    private static final class ProviderRenderer implements AppsCommand.Renderer {
        private final ProviderLink mLink;

        ProviderRenderer(int user) throws PlatformException {
            mLink = new ProviderLink(new ActivityManagerTransport(), user);
        }

        @Override
        public Icon render(Entry entry) throws Exception {
            ActivityInfo info = (ActivityInfo) entry.source;
            List<ResourcePaths.Item> items = resourcePaths(info.applicationInfo);
            if (!ResourcePaths.hasBase(items) || items.size() > IconProtocol.MAX_FILES) {
                throw new PlatformException("the application's resources cannot be opened");
            }
            int[] kinds = new int[items.size()];
            String[] paths = new String[items.size()];
            for (int i = 0; i < kinds.length; i++) {
                switch (items.get(i).kind) {
                    case SPLIT:
                        kinds[i] = IconProtocol.KIND_SPLIT;
                        break;
                    case LIBRARY:
                        kinds[i] = IconProtocol.KIND_LIBRARY;
                        break;
                    case OVERLAY:
                        kinds[i] = IconProtocol.KIND_OVERLAY;
                        break;
                    default:
                        kinds[i] = IconProtocol.KIND_BASE;
                        break;
                }
                paths[i] = items.get(i).path;
            }
            return mLink.render(entry.iconResource, kinds, paths);
        }

        @Override
        public void close() {
            mLink.close();
        }
    }

    /** The provider held and given back through the activity manager, under a token of this hold's own. */
    private static final class ActivityManagerTransport implements ProviderLink.Transport {
        private final IBinder mToken = new Binder();

        @Override
        public ProviderLink.Holding acquire(int user) throws Exception {
            ContentProviderHolder holder = ActivityManager.getService().getContentProviderExternal(
                    IconProtocol.AUTHORITY, user, mToken, AppsCommand.NAME);
            if (holder == null) {
                return null;
            }
            if (holder.provider == null) {
                release(user);
                return null;
            }
            return new HeldProvider(holder);
        }

        @Override
        public void release(int user) {
            try {
                ActivityManager.getService().removeContentProviderExternalAsUser(IconProtocol.AUTHORITY, mToken, user);
            } catch (Exception | LinkageError ignored) {
                // The command ends with its process, and the hold with it.
            }
        }
    }

    /** What the activity manager gave: the provider and, in {@code info}, who it is. */
    private static final class HeldProvider implements ProviderLink.Holding {
        private final ContentProviderHolder mHolder;

        HeldProvider(ContentProviderHolder holder) {
            mHolder = holder;
        }

        @Override
        public String packageName() {
            return mHolder.info == null ? null : mHolder.info.packageName;
        }

        @Override
        public String className() {
            return mHolder.info == null ? null : mHolder.info.name;
        }

        @Override
        public boolean system() {
            return mHolder.info != null && mHolder.info.applicationInfo != null
                    && (mHolder.info.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
        }

        @Override
        public Icon render(int resource, int[] kinds, String[] paths) throws Exception {
            Bundle request = new Bundle();
            request.putInt(IconProtocol.KEY_RESOURCE, resource);
            request.putIntArray(IconProtocol.KEY_KINDS, kinds);
            request.putStringArray(IconProtocol.KEY_PATHS, paths);
            Bundle answer;
            try {
                answer = mHolder.provider.call(new AttributionSource(android.os.Process.myUid(), "com.android.shell", null),
                        IconProtocol.AUTHORITY, IconProtocol.METHOD_RENDER, null, request);
            } catch (DeadObjectException dead) {
                throw new ProviderLink.ProviderDied(dead);
            }
            if (answer == null || answer.getInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR) != IconProtocol.STATUS_OK) {
                return null;
            }
            return new Icon(answer.getInt(IconProtocol.KEY_SIZE), answer.getBoolean(IconProtocol.KEY_ADAPTIVE),
                    answer.getBoolean(IconProtocol.KEY_ALPHA), answer.getByteArray(IconProtocol.KEY_PNG));
        }
    }
}
