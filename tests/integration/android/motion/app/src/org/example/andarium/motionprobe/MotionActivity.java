// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.motionprobe;

import android.app.Activity;
import android.content.Intent;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * A real foreground SensorManager client; never installed in a product image. Mode "events" and mode
 * "movement" read the accelerometer and the gyroscope. Mode "compass" also reads the magnetometer
 * ("Host Magnetometer", an image that lists one), the framework's rotation vector, and writes the
 * compass.json report every 200 ms for as long as it runs.
 */
public final class MotionActivity extends Activity implements SensorEventListener {
    private final Handler handler = new Handler();
    private SensorManager manager;
    private TextView text;
    private Samples acceleration, rotation, field;
    private boolean compass;
    private final float[] gravity = new float[3];
    private final float[] geomagnetic = new float[3];
    private final float[] vector = new float[4];
    private int fieldAccuracy = -1, rotationVectorCount;
    // The magnetometer as the sensor service lists it, and the accuracies onAccuracyChanged was called with.
    private Sensor magnetometer;
    private final java.util.ArrayList<Integer> accuracyCalls = new java.util.ArrayList<>();
    private double fieldAzimuth = Double.NaN, vectorAzimuth = Double.NaN;
    private String mode;
    private String runId;
    private long started, deadline;
    private int seconds;
    private boolean valid, completed;

    private static final class Samples {
        int count;
        long first, last;
        double sum, peak;
        final float[] low = {Float.POSITIVE_INFINITY, Float.POSITIVE_INFINITY, Float.POSITIVE_INFINITY};
        final float[] high = {Float.NEGATIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.NEGATIVE_INFINITY};
        final float[] latest = new float[3];

        boolean add(SensorEvent event) {
            if (event.timestamp <= last || event.timestamp > SystemClock.elapsedRealtimeNanos() + 1_000_000_000L) return false;
            double squared = 0;
            for (int i = 0; i < 3; i++) {
                float value = event.values[i];
                if (!Float.isFinite(value)) return false;
                latest[i] = value;
                low[i] = Math.min(low[i], value);
                high[i] = Math.max(high[i], value);
                squared += (double) value * value;
            }
            double norm = Math.sqrt(squared);
            sum += norm;
            peak = Math.max(peak, norm);
            if (count++ == 0) first = event.timestamp;
            last = event.timestamp;
            return true;
        }

        double span() {
            double span = 0;
            if (count != 0) for (int i = 0; i < 3; i++) span = Math.max(span, high[i] - low[i]);
            return span;
        }

        JSONObject json() throws Exception {
            return new JSONObject().put("count", count).put("first_ns", first).put("last_ns", last)
                .put("mean_norm", count == 0 ? 0 : sum / count).put("peak_norm", peak).put("span", span());
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        text = new TextView(this);
        text.setTextSize(20);
        text.setPadding(28, 28, 28, 28);
        LinearLayout layout = new LinearLayout(this);
        layout.addView(text);
        setContentView(layout);
        manager = getSystemService(SensorManager.class);
        begin(getIntent());
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        begin(intent);
    }

    private void begin(Intent intent) {
        handler.removeCallbacksAndMessages(null);
        manager.unregisterListener(this);
        acceleration = new Samples();
        rotation = new Samples();
        mode = intent.getStringExtra("mode");
        runId = intent.getStringExtra("run_id");
        if (mode == null) mode = "events";
        compass = mode.equals("compass");
        field = new Samples();
        fieldAccuracy = -1;
        rotationVectorCount = 0;
        magnetometer = null;
        accuracyCalls.clear();
        fieldAzimuth = vectorAzimuth = Double.NaN;
        seconds = Math.max(1, Math.min(240, intent.getIntExtra("seconds", 5)));
        started = SystemClock.elapsedRealtimeNanos();
        deadline = started + seconds * 1_000_000_000L;
        completed = false;
        Sensor accel = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        Sensor gyro = manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        valid = accel != null && gyro != null && "Host Accelerometer".equals(accel.getName())
            && "Host Gyroscope".equals(gyro.getName());
        if (valid) valid = manager.registerListener(this, accel, 20_000)
            && manager.registerListener(this, gyro, 20_000);
        if (compass) {
            magnetometer = manager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD);
            valid &= magnetometer != null && "Host Magnetometer".equals(magnetometer.getName());
            if (valid) valid = manager.registerListener(this, magnetometer, intent.getIntExtra("mag_us", 20_000));
            // The framework makes the rotation vector from the three sensors, when all three are listed.
            Sensor vectorSensor = manager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
            if (valid && vectorSensor != null) manager.registerListener(this, vectorSensor, 20_000);
        }
        new File(getFilesDir(), "result.json").delete();
        new File(getFilesDir(), "compass.json").delete();
        handler.post(tick);
    }

    private boolean passed() {
        if (!valid) return false;
        if (compass) return field.count >= 25;
        if (acceleration.count < 75 || rotation.count < 75) return false;
        if (mode.equals("movement")) return acceleration.span() > 0.5 && rotation.peak > 0.05;
        return acceleration.sum / acceleration.count > 7 && acceleration.sum / acceleration.count < 12;
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            boolean done = !valid || SystemClock.elapsedRealtimeNanos() >= deadline
                || (mode.equals("movement") && passed());
            if (compass) writeCompass();
            if (!compass && !completed && done) {
                completed = true;
                try {
                    JSONObject report = new JSONObject().put("mode", mode).put("run_id", runId)
                        .put("valid", valid).put("passed", passed())
                        .put("accelerometer", acceleration.json()).put("gyroscope", rotation.json());
                    File temporary = new File(getFilesDir(), "result.json.new");
                    try (FileOutputStream output = new FileOutputStream(temporary)) {
                        output.write(report.toString().getBytes(StandardCharsets.UTF_8));
                    }
                    if (!temporary.renameTo(new File(getFilesDir(), "result.json"))) throw new Exception("report rename failed");
                } catch (Exception error) { throw new IllegalStateException(error); }
            }
            text.setText(compass ? String.format(Locale.ROOT,
                "Motion Probe\ncompass — %s\n\nMagnetic field (µT)\nx %.3f   y %.3f   z %.3f\nSamples: %d   accuracy %d\nAzimuth %.1f°",
                valid ? "collecting" : "failed", field.latest[0], field.latest[1], field.latest[2], field.count,
                fieldAccuracy, fieldAzimuth) : String.format(Locale.ROOT,
                "Motion Probe\n%s%s\n\nAccelerometer (m/s²)\nx %.3f   y %.3f   z %.3f\nSamples: %d\n\nGyroscope (rad/s)\nx %.3f   y %.3f   z %.3f\nSamples: %d",
                mode, completed ? (passed() ? " — passed" : " — failed") : " — collecting",
                acceleration.latest[0], acceleration.latest[1], acceleration.latest[2], acceleration.count,
                rotation.latest[0], rotation.latest[1], rotation.latest[2], rotation.count));
            handler.postDelayed(this, 200);
        }
    };

    /** The compass report: what the listener has seen of the field, the azimuth it gives, the status. */
    private void writeCompass() {
        try {
            JSONObject report = new JSONObject().put("mode", mode).put("run_id", runId).put("valid", valid)
                .put("started_ns", started)
                .put("accelerometer", acceleration.json()).put("gyroscope", rotation.json())
                .put("magnetometer", field.json().put("accuracy", fieldAccuracy)
                    .put("resolution", magnetometer == null ? JSONObject.NULL : (Object) (double) magnetometer.getResolution())
                    .put("max_range", magnetometer == null ? JSONObject.NULL : (Object) (double) magnetometer.getMaximumRange())
                    .put("accuracy_calls", new org.json.JSONArray(accuracyCalls))
                    .put("latest", new org.json.JSONArray().put(field.latest[0]).put(field.latest[1]).put(field.latest[2]))
                    .put("azimuth_deg", Double.isNaN(fieldAzimuth) ? JSONObject.NULL : fieldAzimuth))
                .put("rotation_vector", new JSONObject().put("count", rotationVectorCount)
                    .put("latest", new org.json.JSONArray().put(vector[0]).put(vector[1]).put(vector[2]).put(vector[3]))
                    .put("azimuth_deg", Double.isNaN(vectorAzimuth) ? JSONObject.NULL : vectorAzimuth));
            File temporary = new File(getFilesDir(), "compass.json.new");
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                output.write(report.toString().getBytes(StandardCharsets.UTF_8));
            }
            if (!temporary.renameTo(new File(getFilesDir(), "compass.json"))) throw new Exception("report rename failed");
        } catch (Exception error) { throw new IllegalStateException(error); }
    }

    /** The azimuth, in degrees from -180 to 180, of the device's y axis from magnetic north. */
    private double azimuth(float[] matrix) {
        float[] angles = new float[3];
        SensorManager.getOrientation(matrix, angles);
        return Math.toDegrees(angles[0]);
    }

    @Override public void onSensorChanged(SensorEvent event) {
        if (event.timestamp < started) return;
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            valid &= acceleration.add(event);
            System.arraycopy(event.values, 0, gravity, 0, 3);
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            valid &= rotation.add(event);
        } else if (compass && event.sensor.getType() == Sensor.TYPE_MAGNETIC_FIELD) {
            valid &= field.add(event);
            fieldAccuracy = event.accuracy;
            System.arraycopy(event.values, 0, geomagnetic, 0, 3);
            float[] matrix = new float[9];
            if (SensorManager.getRotationMatrix(matrix, null, gravity, geomagnetic)) fieldAzimuth = azimuth(matrix);
        } else if (compass && event.sensor.getType() == Sensor.TYPE_ROTATION_VECTOR) {
            rotationVectorCount++;
            System.arraycopy(event.values, 0, vector, 0, 4);
            float[] matrix = new float[9];
            SensorManager.getRotationMatrixFromVector(matrix, vector);
            vectorAzimuth = azimuth(matrix);
        }
    }
    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {
        if (compass && sensor.getType() == Sensor.TYPE_MAGNETIC_FIELD) {
            accuracyCalls.add(accuracy);
            if (accuracyCalls.size() > 64) accuracyCalls.remove(0);
        }
    }
    @Override protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        manager.unregisterListener(this);
        super.onDestroy();
    }
}
