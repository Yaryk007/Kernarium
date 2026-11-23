// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.locationprobe;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.location.GnssStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.location.OnNmeaMessageListener;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.util.Log;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/**
 * A foreground client of the gps provider, the Android half of the GNSS HAL's checks on the
 * candidate image: it reports what an application holding the permission it was granted receives
 * from the host's position, and judges it against the expectation it was launched with. Never
 * installed in a product image.
 *
 * Extras: expect = fine | approximate | coarse | none; lat and lon (doubles, the position the
 * host source reports); seconds (1 to 120); max_accuracy (meters, fine only); run_id; order =
 * locations_first (the default) | status_first.
 * The permission state is the harness's to set (pm grant and pm revoke) before the launch.
 *
 * The order is that of the registrations. locations_first requests the fixes and then registers
 * the status and NMEA listeners while the HAL's first session starts, an order that races the
 * HAL's first callbacks. The framework tells a status listener onStarted only for a session that
 * begins after it registered, and onFirstFix only if it registered before the session's first
 * fix; it replays neither. So only status_first expects them, and only when its session is the
 * first one on the device: another client already holding the gps provider means no session
 * begins for this probe.
 */
public final class LocationActivity extends Activity implements LocationListener {
    private static final String TAG = "LocationProbe";
    private final Handler handler = new Handler();
    private LocationManager manager;
    private TextView text;
    private String expect, runId;
    private double expectedLatitude, expectedLongitude, maxAccuracy;
    private long deadline;
    private boolean completed, gnssDenied, nmeaDenied, statusFirst;

    // What the location listener saw.
    private int fixes;
    private double minAccuracy, maxAccuracySeen, maxDistance;
    private boolean anyAltitude, anySpeed, anyBearing, onlyGps, anyMock;
    private long newestAgeMs, oldestAgeMs;
    private double lastLatitude, lastLongitude, lastAccuracy;
    // What the GnssStatus callback saw.
    private boolean started, stopped;
    private int firstFixes, satelliteCallbacks, maxSatellites;
    // What the NMEA listener saw.
    private int sentences;
    private final Set<String> ggaQualities = new HashSet<>();
    private boolean nmeaAltitude, nmeaSpeed;
    private final JSONArray sample = new JSONArray();

    private final GnssStatus.Callback gnssCallback = new GnssStatus.Callback() {
        @Override public void onStarted() { started = true; }
        @Override public void onStopped() { stopped = true; }
        @Override public void onFirstFix(int ttffMillis) { firstFixes++; }
        @Override public void onSatelliteStatusChanged(GnssStatus status) {
            satelliteCallbacks++;
            maxSatellites = Math.max(maxSatellites, status.getSatelliteCount());
        }
    };

    private final OnNmeaMessageListener nmeaListener = (message, timestamp) -> {
        sentences++;
        if (sample.length() < 12) sample.put(message.trim());
        String[] fields = message.split(",", -1);
        if (fields[0].endsWith("GGA") && fields.length > 9) {
            ggaQualities.add(fields[6]);
            nmeaAltitude |= !fields[9].isEmpty();
        } else if (fields[0].endsWith("RMC") && fields.length > 8) {
            nmeaSpeed |= !fields[7].isEmpty() || !fields[8].isEmpty();
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        text = new TextView(this);
        text.setTextSize(18);
        text.setPadding(28, 28, 28, 28);
        LinearLayout layout = new LinearLayout(this);
        layout.addView(text);
        setContentView(layout);
        manager = getSystemService(LocationManager.class);
        begin(getIntent());
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        begin(intent);
    }

    private void stopListening() {
        handler.removeCallbacksAndMessages(null);
        manager.removeUpdates(this);
        manager.unregisterGnssStatusCallback(gnssCallback);
        manager.removeNmeaListener(nmeaListener);
    }

    private void begin(Intent intent) {
        stopListening();
        expect = intent.getStringExtra("expect");
        if (expect == null) expect = "fine";
        runId = intent.getStringExtra("run_id");
        expectedLatitude = intent.getDoubleExtra("lat", 0);
        expectedLongitude = intent.getDoubleExtra("lon", 0);
        maxAccuracy = intent.getDoubleExtra("max_accuracy", 50);
        int seconds = Math.max(1, Math.min(120, intent.getIntExtra("seconds", 15)));
        deadline = SystemClock.elapsedRealtime() + seconds * 1000L;
        completed = gnssDenied = nmeaDenied = false;
        statusFirst = "status_first".equals(intent.getStringExtra("order"));
        fixes = firstFixes = satelliteCallbacks = maxSatellites = sentences = 0;
        minAccuracy = Double.MAX_VALUE;
        maxAccuracySeen = maxDistance = 0;
        anyAltitude = anySpeed = anyBearing = anyMock = started = stopped = nmeaAltitude = nmeaSpeed = false;
        onlyGps = true;
        ggaQualities.clear();
        for (int i = sample.length() - 1; i >= 0; i--) sample.remove(i);
        new File(getFilesDir(), "result.json").delete();
        if (statusFirst) listenToStatus();
        try {
            manager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, this, getMainLooper());
        } catch (SecurityException | IllegalArgumentException error) {
            Log.e(TAG, "requestLocationUpdates", error);
        }
        if (!statusFirst) listenToStatus();
        handler.post(tick);
    }

    // Satellite status and NMEA need the precise permission; an application with the coarse
    // one is refused, which is part of what the coarse expectation checks.
    private void listenToStatus() {
        try {
            manager.registerGnssStatusCallback(getMainExecutor(), gnssCallback);
        } catch (SecurityException error) {
            gnssDenied = true;
        }
        try {
            manager.addNmeaListener(getMainExecutor(), nmeaListener);
        } catch (SecurityException error) {
            nmeaDenied = true;
        }
    }

    // onStarted is owed only to a listener that registered before the session began, and
    // onFirstFix to one that registered before the first fix.
    private boolean startReported() {
        return started || !statusFirst;
    }

    private boolean firstFixReported() {
        return firstFixes >= 1 || !statusFirst;
    }

    @Override public void onLocationChanged(Location location) {
        float[] distance = new float[1];
        Location.distanceBetween(expectedLatitude, expectedLongitude, location.getLatitude(),
            location.getLongitude(), distance);
        fixes++;
        maxDistance = Math.max(maxDistance, distance[0]);
        if (location.hasAccuracy()) {
            minAccuracy = Math.min(minAccuracy, location.getAccuracy());
            maxAccuracySeen = Math.max(maxAccuracySeen, location.getAccuracy());
        } else {
            maxAccuracySeen = Double.MAX_VALUE;
        }
        anyAltitude |= location.hasAltitude();
        anySpeed |= location.hasSpeed();
        anyBearing |= location.hasBearing();
        anyMock |= location.isMock();
        onlyGps &= LocationManager.GPS_PROVIDER.equals(location.getProvider());
        long age = (SystemClock.elapsedRealtimeNanos() - location.getElapsedRealtimeNanos()) / 1_000_000;
        if (fixes == 1) oldestAgeMs = age;
        newestAgeMs = age;
        lastLatitude = location.getLatitude();
        lastLongitude = location.getLongitude();
        lastAccuracy = location.getAccuracy();
    }

    private boolean passed() {
        boolean noPrecision = !anyAltitude && !anySpeed && !anyBearing;
        switch (expect) {
            case "fine":
                return fixes >= 3 && maxDistance <= 10 && maxAccuracySeen <= maxAccuracy && onlyGps && !anyMock
                    && startReported() && firstFixReported() && satelliteCallbacks >= 1 && maxSatellites == 0
                    && !gnssDenied && !nmeaDenied
                    && ggaQualities.contains("1") && newestAgeMs < 30_000;
            case "approximate":
                // The host source reports an approximate fix; this application holds the precise
                // permission and must still get no more than a wide circle.
                return fixes >= 1 && maxDistance <= 10 && minAccuracy >= 2000 && noPrecision && onlyGps
                    && !gnssDenied && ggaQualities.contains("6") && !ggaQualities.contains("1")
                    && !nmeaAltitude && !nmeaSpeed;
            case "coarse":
                // Android's own coarse location of the fix: a grid, an offset, no precision, and no
                // satellite status or sentences for an application without the precise permission.
                return fixes >= 1 && minAccuracy >= 2000 && noPrecision && maxDistance <= 6000
                    && gnssDenied && nmeaDenied;
            case "none":
                // The receiver has no position: it powers up, searches and reports nothing.
                return fixes == 0 && startReported() && !gnssDenied && sentences >= 6
                    && ggaQualities.size() == 1 && ggaQualities.contains("0");
            default:
                return false;
        }
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!completed && SystemClock.elapsedRealtime() >= deadline) {
                completed = true;
                stopListening();
                try {
                    JSONObject report = new JSONObject().put("expect", expect).put("run_id", runId)
                        .put("order", statusFirst ? "status_first" : "locations_first")
                        .put("passed", passed())
                        .put("provider_enabled", manager.isProviderEnabled(LocationManager.GPS_PROVIDER))
                        .put("feature_gps", getPackageManager().hasSystemFeature(PackageManager.FEATURE_LOCATION_GPS))
                        .put("fixes", fixes).put("min_accuracy", fixes == 0 ? JSONObject.NULL : minAccuracy)
                        .put("max_accuracy", fixes == 0 ? JSONObject.NULL : maxAccuracySeen)
                        .put("max_distance_m", maxDistance).put("altitude", anyAltitude).put("speed", anySpeed)
                        .put("bearing", anyBearing).put("only_gps", onlyGps).put("mock", anyMock)
                        .put("age_ms_first", oldestAgeMs).put("age_ms_last", newestAgeMs)
                        .put("gnss_denied", gnssDenied).put("nmea_denied", nmeaDenied)
                        .put("gnss_started", started).put("gnss_stopped", stopped).put("first_fixes", firstFixes)
                        .put("satellite_callbacks", satelliteCallbacks).put("max_satellites", maxSatellites)
                        .put("nmea_sentences", sentences).put("gga_qualities", new JSONArray(ggaQualities))
                        .put("nmea_altitude", nmeaAltitude).put("nmea_speed", nmeaSpeed)
                        .put("nmea_sample", sample);
                    File temporary = new File(getFilesDir(), "result.json.new");
                    try (FileOutputStream output = new FileOutputStream(temporary)) {
                        output.write(report.toString().getBytes(StandardCharsets.UTF_8));
                    }
                    if (!temporary.renameTo(new File(getFilesDir(), "result.json"))) throw new Exception("report rename failed");
                } catch (Exception error) { throw new IllegalStateException(error); }
            }
            text.setText(String.format(Locale.ROOT,
                "Location Probe\n%s%s\n\nFixes: %d\nLast: %.5f, %.5f\nAccuracy: %.0f m\nSatellite callbacks: %d (max %d)\nNMEA sentences: %d",
                expect, completed ? (passed() ? " - passed" : " - failed") : " - collecting", fixes,
                lastLatitude, lastLongitude, lastAccuracy, satelliteCallbacks, maxSatellites, sentences));
            handler.postDelayed(this, 500);
        }
    };

    @Override protected void onDestroy() {
        stopListening();
        super.onDestroy();
    }
}
