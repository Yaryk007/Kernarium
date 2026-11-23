// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.keymouseprobe;

import android.app.Activity;
import android.content.Intent;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Locale;

/**
 * A foreground client of the input stack, the Android half of the hardware keyboard and mouse
 * checks on the candidate image: it logs every input device Android has, every key, every pointer
 * event it receives while it is in front, and the state of pointer capture, one line each, to
 * files/events.log in its data directory and to logcat under the tag KeyMouseProbe. Never
 * installed in a product image.
 *
 * Line format: "<sequence> <kind> key=value ...", a value with a space is quoted. Kinds: begin,
 * device, device-added, device-removed, device-changed, key, hover, touch, button, scroll,
 * captured, capture, capture-request. Positions are in screen pixels.
 *
 * Extras (on the launch and on every later `am start`, which the activity takes as a new intent):
 * capture (boolean) requests pointer capture, or releases it; run_id (string) is logged. The
 * activity holds the screen on and takes the focus, so capture can be granted.
 */
public final class KeyMouseActivity extends Activity implements InputManager.InputDeviceListener {
    private static final String TAG = "KeyMouseProbe";
    private static final int RECENT_LINES = 14;

    private final Object lock = new Object();
    private final ArrayDeque<String> recent = new ArrayDeque<>();
    private int sequence;
    private File log;
    private TextView view;
    private InputManager inputManager;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        view = new TextView(this);
        view.setTextSize(14);
        view.setPadding(24, 24, 24, 24);
        view.setFocusable(true);
        view.setFocusableInTouchMode(true);
        // Captured events (relative motion, buttons and scroll while the window holds pointer
        // capture) go to the focused view's listener.
        view.setOnCapturedPointerListener((v, event) -> {
            describeMotion("captured", event);
            return true;
        });
        setContentView(view);
        view.requestFocus();
        log = new File(getFilesDir(), "events.log");
        log.delete();
        inputManager = getSystemService(InputManager.class);
        inputManager.registerInputDeviceListener(this, null);
        emit("begin", "run_id=" + quote(String.valueOf(getIntent().getStringExtra("run_id"))));
        for (int id : InputDevice.getDeviceIds()) describeDevice("device", id);
        take(getIntent());
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        take(intent);
    }

    @Override protected void onDestroy() {
        inputManager.unregisterInputDeviceListener(this);
        super.onDestroy();
    }

    private void take(Intent intent) {
        if (!intent.hasExtra("capture")) return;
        boolean enable = intent.getBooleanExtra("capture", false);
        emit("capture-request", "enable=" + enable + " focus=" + hasWindowFocus());
        // The window needs the focus to be granted capture; the view is asked after the layout.
        view.post(() -> {
            if (enable) view.requestPointerCapture();
            else view.releasePointerCapture();
        });
    }

    @Override public void onPointerCaptureChanged(boolean hasCapture) {
        super.onPointerCaptureChanged(hasCapture);
        emit("capture", "state=" + (hasCapture ? "on" : "off"));
    }

    // ---- Devices.

    @Override public void onInputDeviceAdded(int id) { describeDevice("device-added", id); }
    @Override public void onInputDeviceRemoved(int id) { emit("device-removed", "id=" + id); }
    @Override public void onInputDeviceChanged(int id) { describeDevice("device-changed", id); }

    private void describeDevice(String kind, int id) {
        InputDevice device = InputDevice.getDevice(id);
        if (device == null) return;
        emit(kind, String.format(Locale.ROOT,
                "id=%d name=%s vendor=0x%04x product=0x%04x sources=0x%08x keyboard_type=%d virtual=%b",
                id, quote(device.getName()), device.getVendorId(), device.getProductId(),
                device.getSources(), device.getKeyboardType(), device.isVirtual()));
    }

    // ---- Events.

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        emit("key", String.format(Locale.ROOT,
                "action=%s code=%s scan=%d device=%d repeat=%d meta=0x%x",
                event.getAction() == KeyEvent.ACTION_DOWN ? "down" : "up",
                KeyEvent.keyCodeToString(event.getKeyCode()), event.getScanCode(),
                event.getDeviceId(), event.getRepeatCount(), event.getMetaState()));
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        describeMotion(null, event);
        return super.dispatchGenericMotionEvent(event);
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        describeMotion(null, event);
        // A mouse click is the probe's to take: left unhandled, Android turns a secondary click
        // into Back, which would end the probe.
        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) return true;
        return super.dispatchTouchEvent(event);
    }

    private void describeMotion(String captured, MotionEvent event) {
        int action = event.getActionMasked();
        String kind;
        String name;
        switch (action) {
            case MotionEvent.ACTION_HOVER_ENTER: kind = "hover"; name = "enter"; break;
            case MotionEvent.ACTION_HOVER_MOVE: kind = "hover"; name = "move"; break;
            case MotionEvent.ACTION_HOVER_EXIT: kind = "hover"; name = "exit"; break;
            case MotionEvent.ACTION_SCROLL: kind = "scroll"; name = "scroll"; break;
            case MotionEvent.ACTION_BUTTON_PRESS: kind = "button"; name = "press"; break;
            case MotionEvent.ACTION_BUTTON_RELEASE: kind = "button"; name = "release"; break;
            case MotionEvent.ACTION_DOWN: kind = "touch"; name = "down"; break;
            case MotionEvent.ACTION_MOVE: kind = "touch"; name = "move"; break;
            case MotionEvent.ACTION_UP: kind = "touch"; name = "up"; break;
            case MotionEvent.ACTION_CANCEL: kind = "touch"; name = "cancel"; break;
            default: kind = "touch"; name = "action" + action; break;
        }
        if (captured != null) kind = captured;
        // Captured events carry relative motion in x and y; the others, the position on screen.
        float x = captured != null ? event.getX() : event.getRawX();
        float y = captured != null ? event.getY() : event.getRawY();
        emit(kind, String.format(Locale.ROOT,
                "action=%s x=%.1f y=%.1f rel_x=%.1f rel_y=%.1f vscroll=%.2f hscroll=%.2f buttons=0x%x "
                + "action_button=0x%x tool=%d source=0x%08x device=%d",
                name, x, y, event.getAxisValue(MotionEvent.AXIS_RELATIVE_X),
                event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y),
                event.getAxisValue(MotionEvent.AXIS_VSCROLL),
                event.getAxisValue(MotionEvent.AXIS_HSCROLL), event.getButtonState(),
                event.getActionButton(), event.getToolType(0), event.getSource(),
                event.getDeviceId()));
    }

    // ---- The log.

    private static String quote(String text) {
        return "\"" + text.replace("\"", "'") + "\"";
    }

    private void emit(String kind, String fields) {
        String line;
        synchronized (lock) {
            line = (++sequence) + " " + kind + " " + fields;
            try (FileOutputStream stream = new FileOutputStream(log, true)) {
                stream.write((line + "\n").getBytes(StandardCharsets.UTF_8));
            } catch (java.io.IOException error) {
                Log.e(TAG, "events.log", error);
            }
            recent.addLast(line);
            while (recent.size() > RECENT_LINES) recent.removeFirst();
        }
        Log.i(TAG, line);
        runOnUiThread(() -> {
            StringBuilder shown = new StringBuilder();
            synchronized (lock) {
                for (String each : recent) shown.append(each).append('\n');
            }
            view.setText(shown);
        });
    }
}
