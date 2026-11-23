// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.gamepadprobe;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Typeface;
import android.hardware.BatteryState;
import android.hardware.input.InputManager;
import android.hardware.lights.Light;
import android.hardware.lights.LightState;
import android.hardware.lights.LightsManager;
import android.hardware.lights.LightsRequest;
import android.os.Bundle;
import android.os.CombinedVibration;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.VibratorManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * A foreground game controller client. It records what Android delivers from the controllers (key and motion
 * events, device added and removed callbacks), describes each controller through the public InputDevice API
 * (identity, keys, motion ranges, vibrators, battery, lights), and vibrates and sets lights on command. The
 * harness drives it with broadcasts to ACTION and reads files/state.json through run-as; see README.md.
 *
 * <p>Commands, all with the string extra "cmd":
 * <ul>
 * <li>"reset": forget the events and the listener log;
 * <li>"record" (boolean "on"): turn the event log off or on, the counters always count;
 * <li>"dump" (string "nonce"): write files/state.json, which carries the nonce;
 * <li>"vibrate" (int "device", ints "strong", "weak" as amplitudes 0 to 255, int "ms", string "mode" of
 *     "motors" or "device"): "motors" drives the two vibrators separately, "device" drives the device's own
 *     vibrator;
 * <li>"cancel" (int "device"): cancel the vibration;
 * <li>"light" (int "device", string "which" of "rgb" or "player", int "color" or "id"): request one light;
 * <li>"release": close the lights session.
 * </ul>
 */
public class GamepadActivity extends Activity {
    static final String ACTION = "org.example.andarium.gamepadprobe.COMMAND";
    private static final int MAXIMUM_EVENTS = 50000;
    private static final int SCREEN_LINES = 14;

    private static final int[] AXES = {
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
    };
    private static final String[] AXIS_NAMES = {"x", "y", "z", "rz", "ltrigger", "rtrigger", "hat_x", "hat_y"};
    private static final int[] KEYS = {
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_THUMBL,
        KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
        KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
    };

    private InputManager inputManager;
    private TextView screen;
    private final List<JSONObject> events = new ArrayList<>();
    private final List<JSONObject> listenerLog = new ArrayList<>();
    private final List<String> screenLines = new ArrayList<>();
    private boolean recording = true;
    private long keyCount;
    private long motionSampleCount;
    private String nonce = "";
    private LightsManager.LightsSession lightsSession;

    private final InputManager.InputDeviceListener listener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int deviceId) { logListener("added", deviceId); }
        @Override public void onInputDeviceRemoved(int deviceId) { logListener("removed", deviceId); }
        @Override public void onInputDeviceChanged(int deviceId) { logListener("changed", deviceId); }
    };

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            try {
                command(intent);
            } catch (RuntimeException | JSONException | IOException problem) {
                showLine("command failed: " + problem);
                failure = String.valueOf(problem);
            }
        }
    };
    private String failure = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setShowWhenLocked(true);
        setTurnScreenOn(true);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        screen = new TextView(this);
        screen.setTypeface(Typeface.MONOSPACE);
        screen.setTextSize(11);
        screen.setPadding(16, 48, 16, 16);
        setContentView(screen);
        inputManager = getSystemService(InputManager.class);
        inputManager.registerInputDeviceListener(listener, null);
        registerReceiver(receiver, new IntentFilter(ACTION), Context.RECEIVER_EXPORTED);
        showLine("ready");
    }

    @Override
    protected void onDestroy() {
        inputManager.unregisterInputDeviceListener(listener);
        unregisterReceiver(receiver);
        if (lightsSession != null) {
            lightsSession.close();
            lightsSession = null;
        }
        super.onDestroy();
    }

    // ---- Events. Both handlers consume what comes from a game controller, so nothing reaches the system's
    // handling of BACK or HOME for them.

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (!(event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_DPAD))) {
            return super.dispatchKeyEvent(event);
        }
        keyCount++;
        if (recording) {
            long nowWall = System.currentTimeMillis();
            long age = SystemClock.uptimeMillis() - event.getEventTime();
            try {
                JSONObject entry = new JSONObject();
                entry.put("kind", "key");
                entry.put("device", event.getDeviceId());
                entry.put("code", event.getKeyCode());
                entry.put("action", event.getAction() == KeyEvent.ACTION_DOWN ? "down" : "up");
                entry.put("repeat", event.getRepeatCount());
                entry.put("kernelMs", nowWall - age);
                entry.put("receivedMs", nowWall);
                add(entry);
            } catch (JSONException problem) {
                throw new IllegalStateException(problem);
            }
            showLine(KeyEvent.keyCodeToString(event.getKeyCode()) + (event.getAction() == KeyEvent.ACTION_DOWN
                    ? " down" : " up") + " dev " + event.getDeviceId());
        }
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return super.dispatchGenericMotionEvent(event);
        }
        long nowWall = System.currentTimeMillis();
        long nowUptime = SystemClock.uptimeMillis();
        int history = event.getHistorySize();
        motionSampleCount += history + 1;
        if (!recording) {
            return true;
        }
        try {
            // Android batches motion events: each sample of the history has its own time.
            for (int i = 0; i <= history; i++) {
                long eventTime = i < history ? event.getHistoricalEventTime(i) : event.getEventTime();
                JSONObject entry = new JSONObject();
                entry.put("kind", "motion");
                entry.put("device", event.getDeviceId());
                for (int axis = 0; axis < AXES.length; axis++) {
                    double value = i < history ? event.getHistoricalAxisValue(AXES[axis], i)
                            : event.getAxisValue(AXES[axis]);
                    entry.put(AXIS_NAMES[axis], value);
                }
                entry.put("kernelMs", nowWall - (nowUptime - eventTime));
                entry.put("receivedMs", nowWall);
                add(entry);
            }
        } catch (JSONException problem) {
            throw new IllegalStateException(problem);
        }
        showLine(String.format("axes dev %d x %.2f y %.2f z %.2f rz %.2f lt %.2f rt %.2f hat %.0f,%.0f",
                event.getDeviceId(), event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y),
                event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ),
                event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_HAT_X), event.getAxisValue(MotionEvent.AXIS_HAT_Y)));
        return true;
    }

    private void add(JSONObject entry) {
        if (events.size() < MAXIMUM_EVENTS) {
            events.add(entry);
        }
    }

    private void logListener(String kind, int deviceId) {
        try {
            JSONObject entry = new JSONObject();
            entry.put("kind", kind);
            entry.put("id", deviceId);
            entry.put("ms", System.currentTimeMillis());
            listenerLog.add(entry);
        } catch (JSONException problem) {
            throw new IllegalStateException(problem);
        }
        showLine("device " + kind + " " + deviceId);
    }

    private void showLine(String line) {
        screenLines.add(line);
        while (screenLines.size() > SCREEN_LINES) {
            screenLines.remove(0);
        }
        if (screen != null) {
            screen.setText(String.join("\n", screenLines));
        }
    }

    // ---- Commands.

    private void command(Intent intent) throws JSONException, IOException {
        String name = intent.getStringExtra("cmd");
        if (name == null) {
            return;
        }
        switch (name) {
            case "reset":
                events.clear();
                listenerLog.clear();
                keyCount = 0;
                motionSampleCount = 0;
                failure = "";
                break;
            case "record":
                recording = intent.getBooleanExtra("on", true);
                break;
            case "dump":
                nonce = String.valueOf(intent.getStringExtra("nonce"));
                writeState();
                break;
            case "vibrate":
                vibrate(intent);
                break;
            case "cancel": {
                InputDevice device = InputDevice.getDevice(intent.getIntExtra("device", -1));
                if (device != null) {
                    device.getVibratorManager().cancel();
                }
                break;
            }
            case "light":
                light(intent);
                break;
            case "release":
                if (lightsSession != null) {
                    lightsSession.close();
                    lightsSession = null;
                }
                break;
            default:
                failure = "unknown command " + name;
        }
    }

    private void vibrate(Intent intent) {
        InputDevice device = InputDevice.getDevice(intent.getIntExtra("device", -1));
        if (device == null) {
            failure = "vibrate: no such device";
            return;
        }
        int strong = intent.getIntExtra("strong", 0);
        int weak = intent.getIntExtra("weak", 0);
        long milliseconds = intent.getIntExtra("ms", 300);
        VibratorManager manager = device.getVibratorManager();
        int[] ids = manager.getVibratorIds();
        if ("device".equals(intent.getStringExtra("mode")) || ids.length < 2) {
            manager.vibrate(CombinedVibration.createParallel(VibrationEffect.createOneShot(milliseconds, strong)));
            return;
        }
        CombinedVibration.ParallelCombination parallel = CombinedVibration.startParallel();
        // Vibrator 0 is the strong (left) motor and vibrator 1 the weak (right) one; an amplitude of 0 is
        // left out, because a one-shot effect has none.
        if (strong > 0) {
            parallel.addVibrator(ids[0], VibrationEffect.createOneShot(milliseconds, strong));
        }
        if (weak > 0) {
            parallel.addVibrator(ids[1], VibrationEffect.createOneShot(milliseconds, weak));
        }
        if (strong <= 0 && weak <= 0) {
            manager.cancel();
            return;
        }
        manager.vibrate(parallel.combine());
    }

    private void light(Intent intent) {
        InputDevice device = InputDevice.getDevice(intent.getIntExtra("device", -1));
        if (device == null) {
            failure = "light: no such device";
            return;
        }
        LightsManager manager = device.getLightsManager();
        if (lightsSession != null) {
            lightsSession.close();
        }
        lightsSession = manager.openSession();
        boolean rgb = "rgb".equals(intent.getStringExtra("which"));
        LightsRequest.Builder request = new LightsRequest.Builder();
        boolean found = false;
        for (Light light : manager.getLights()) {
            // The player id light is the one without a colour; the lightbar has one.
            if (rgb && light.hasRgbControl()) {
                request.addLight(light, new LightState.Builder().setColor(intent.getIntExtra("color", 0)).build());
                found = true;
                break;
            }
            if (!rgb && !light.hasRgbControl()) {
                request.addLight(light, new LightState.Builder().setPlayerId(intent.getIntExtra("id", 0)).build());
                found = true;
                break;
            }
        }
        if (!found) {
            failure = "light: the device has no " + (rgb ? "colour" : "player id") + " light";
            return;
        }
        lightsSession.requestLights(request.build());
    }

    // ---- The state file.

    private JSONObject describe(InputDevice device) throws JSONException {
        JSONObject entry = new JSONObject();
        entry.put("id", device.getId());
        entry.put("name", device.getName());
        entry.put("vendor", device.getVendorId());
        entry.put("product", device.getProductId());
        entry.put("sources", device.getSources());
        entry.put("controllerNumber", device.getControllerNumber());
        entry.put("descriptor", device.getDescriptor());
        entry.put("external", device.isExternal());
        boolean[] has = device.hasKeys(KEYS);
        JSONArray keys = new JSONArray();
        for (int i = 0; i < KEYS.length; i++) {
            if (has[i]) {
                keys.put(KEYS[i]);
            }
        }
        entry.put("keys", keys);
        JSONArray ranges = new JSONArray();
        for (InputDevice.MotionRange range : device.getMotionRanges()) {
            JSONObject item = new JSONObject();
            item.put("axis", MotionEvent.axisToString(range.getAxis()));
            item.put("source", range.getSource());
            item.put("min", range.getMin());
            item.put("max", range.getMax());
            item.put("flat", range.getFlat());
            item.put("fuzz", range.getFuzz());
            ranges.put(item);
        }
        entry.put("ranges", ranges);
        entry.put("vibrators", device.getVibratorManager().getVibratorIds().length);
        BatteryState battery = device.getBatteryState();
        JSONObject state = new JSONObject();
        state.put("present", battery.isPresent());
        float capacity = battery.getCapacity();
        state.put("capacity", Float.isNaN(capacity) ? JSONObject.NULL : (double) capacity);
        state.put("status", battery.getStatus());
        entry.put("battery", state);
        JSONArray lights = new JSONArray();
        for (Light light : device.getLightsManager().getLights()) {
            JSONObject item = new JSONObject();
            item.put("id", light.getId());
            item.put("name", light.getName());
            item.put("ordinal", light.getOrdinal());
            item.put("type", light.getType());
            item.put("rgb", light.hasRgbControl());
            item.put("brightness", light.hasBrightnessControl());
            lights.put(item);
        }
        entry.put("lights", lights);
        return entry;
    }

    private void writeState() throws JSONException, IOException {
        JSONObject root = new JSONObject();
        root.put("nonce", nonce);
        root.put("failure", failure);
        root.put("uptimeMs", SystemClock.uptimeMillis());
        root.put("wallMs", System.currentTimeMillis());
        root.put("keyCount", keyCount);
        root.put("motionSampleCount", motionSampleCount);
        root.put("recording", recording);
        JSONArray devices = new JSONArray();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && (device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                    || device.supportsSource(InputDevice.SOURCE_JOYSTICK))) {
                devices.put(describe(device));
            }
        }
        root.put("devices", devices);
        root.put("events", new JSONArray(events));
        root.put("listener", new JSONArray(listenerLog));
        File directory = getFilesDir();
        File temporary = new File(directory, "state.json.tmp");
        try (FileOutputStream stream = new FileOutputStream(temporary)) {
            stream.write(root.toString().getBytes(StandardCharsets.UTF_8));
        }
        if (!temporary.renameTo(new File(directory, "state.json"))) {
            throw new IOException("cannot rename the state file");
        }
    }
}
